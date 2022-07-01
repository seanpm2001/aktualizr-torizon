#include <boost/filesystem/path.hpp>
#include <sstream>

#include "dockercomposesecondary.h"
#include "dockerofflineloader.h"
#include "uptane/manifest.h"
#include "libaktualizr/types.h"
#include "logging/logging.h"
#include "utilities/fault_injection.h"
#include "utilities/utils.h"
#include "compose_manager.h"
#include "storage/invstorage.h"

using std::stringstream;

namespace bpo = boost::program_options;

namespace Primary {

const char* const DockerComposeSecondaryConfig::Type = "docker-compose";

DockerComposeSecondaryConfig::DockerComposeSecondaryConfig(const Json::Value& json_config) : ManagedSecondaryConfig(Type) {
  partial_verifying = json_config["partial_verifying"].asBool();
  ecu_serial = json_config["ecu_serial"].asString();
  ecu_hardware_id = json_config["ecu_hardware_id"].asString();
  full_client_dir = json_config["full_client_dir"].asString();
  ecu_private_key = json_config["ecu_private_key"].asString();
  ecu_public_key = json_config["ecu_public_key"].asString();
  firmware_path = json_config["firmware_path"].asString();
  target_name_path = json_config["target_name_path"].asString();
  metadata_path = json_config["metadata_path"].asString();
}

std::vector<DockerComposeSecondaryConfig> DockerComposeSecondaryConfig::create_from_file(
    const boost::filesystem::path& file_full_path) {
  Json::Value json_config;
  std::ifstream json_file(file_full_path.string());
  Json::parseFromStream(Json::CharReaderBuilder(), json_file, &json_config, nullptr);
  json_file.close();

  std::vector<DockerComposeSecondaryConfig> sec_configs;
  sec_configs.reserve(json_config[Type].size());

  for (const auto& item : json_config[Type]) {
    sec_configs.emplace_back(DockerComposeSecondaryConfig(item));
  }
  return sec_configs;
}

void DockerComposeSecondaryConfig::dump(const boost::filesystem::path& file_full_path) const {
  Json::Value json_config;

  json_config["partial_verifying"] = partial_verifying;
  json_config["ecu_serial"] = ecu_serial;
  json_config["ecu_hardware_id"] = ecu_hardware_id;
  json_config["full_client_dir"] = full_client_dir.string();
  json_config["ecu_private_key"] = ecu_private_key;
  json_config["ecu_public_key"] = ecu_public_key;
  json_config["firmware_path"] = firmware_path.string();
  json_config["target_name_path"] = target_name_path.string();
  json_config["metadata_path"] = metadata_path.string();

  Json::Value root;
  root[Type].append(json_config);

  Json::StreamWriterBuilder json_bwriter;
  json_bwriter["indentation"] = "\t";
  std::unique_ptr<Json::StreamWriter> const json_writer(json_bwriter.newStreamWriter());

  boost::filesystem::create_directories(file_full_path.parent_path());
  std::ofstream json_file(file_full_path.string());
  json_writer->write(root, &json_file);
  json_file.close();
}

DockerComposeSecondary::DockerComposeSecondary(Primary::DockerComposeSecondaryConfig sconfig_in)
    : ManagedSecondary(std::move(sconfig_in)) {
  validateInstall();
}

/*
 * TODO: As per https://gitlab.int.toradex.com/rd/torizon-core/aktualizr/-/merge_requests/7#note_70291
 * we may need to override method sendFirmware() possibly also passing the `info` parameter and
 * performing operations that can fail inside that method.
 *
 * As to the question of what is the part that could fail (review later):
 *
 * - For online updates this would probably involve pulling the images.
 * - For offline updates it would likely involve validating the Docker images?
 */

data::InstallationResult DockerComposeSecondary::install(const Uptane::Target &target, const InstallInfo& info) {
  auto tgt_stream = secondary_provider_->getTargetFileHandle(target);

  /* Here we try to make container updates "as atomic as possible". So we save
   * the updated docker-compose file with another name (<firmware_path>.tmp), run
   * docker-compose commands to pull and run the containers, and if it fails
   * we still have the previous docker-compose file to "rollback" to the current
   * version of the containers.
   */

  bool update_status = false;
  std::string compose_cur = sconfig.firmware_path.string();
  std::string compose_new = compose_cur + ".tmp";
  // Just a temp file used to atomically write the "tmp" compose file
  std::string compose_temp = compose_cur + ".temporary";

  ComposeManager compose = ComposeManager(compose_cur, compose_new);
  bool sync_update = pendingPrimaryUpdate();

  // Save new compose file in a temporary file.
  std::ofstream out_file(compose_temp, std::ios::binary);
  out_file << tgt_stream.rdbuf();
  tgt_stream.close();
  out_file.close();
  rename(compose_temp.c_str(), compose_new.c_str());

  if (info.getUpdateType() == UpdateType::kOnline) {
    // Run online update method.
    update_status = compose.update(false, sync_update);

  } else if (info.getUpdateType() == UpdateType::kOffline) {
    auto img_path = info.getImagesPathOffline() / (target.sha256Hash() + ".images");
    auto man_path = info.getMetadataPathOffline() / "docker" / (target.sha256Hash() + ".manifests");
    boost::filesystem::path compose_out;

    if (loadDockerImages(compose_new, target.sha256Hash(), img_path, man_path, &compose_out)) {
      // Docker images loaded and an "offline" version of compose-file available.
      // Overwrite the new compose file with that "offline" version.
      boost::filesystem::rename(compose_out, compose_new);

      update_status = compose.update(true, sync_update);
    } else {
      compose.sync_update = sync_update;
    }

  } else {
    return data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, "Unknown update type");
  }

  if (update_status == true) {
    Utils::writeFile(sconfig.target_name_path, target.filename());
    if (sync_update) {
      return data::InstallationResult(data::ResultCode::Numeric::kNeedCompletion, "");
    } else {
      return data::InstallationResult(data::ResultCode::Numeric::kOk, "");
    }
  } else {
    compose.rollback();
    return data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, "");
  }
}

bool DockerComposeSecondary::loadDockerImages(const boost::filesystem::path &compose_in,
                                              const std::string &compose_sha256,
                                              const boost::filesystem::path &images_path,
                                              const boost::filesystem::path &manifests_path,
                                              boost::filesystem::path *compose_out) {
  if (compose_out != nullptr) { compose_out->clear(); }

  boost::filesystem::path compose_new = compose_in;
  compose_new.replace_extension(".off");

  try {
    auto dmcache = std::make_shared<DockerManifestsCache>(manifests_path);

    DockerComposeOfflineLoader dcloader(images_path, dmcache);
    dcloader.loadCompose(compose_in, compose_sha256);
    dcloader.dumpReferencedImages();
    dcloader.dumpImageMapping();
    dcloader.installImages();
    dcloader.writeOfflineComposeFile(compose_new);
    // TODO: [OFFUPD] Define how to perform the offline-online transformation (related to getFirmwareInfo()).

  } catch (std::runtime_error &exc) {
    // TODO: Consider throwing/handling custom exception types from dockerofflineloader and dockertarballloader.
    LOG_WARNING << "Offline loading failed: " << exc.what();
    return false;
  }

  if (compose_out != nullptr) { *compose_out = compose_new; }

  return true;
}

bool DockerComposeSecondary::pendingPrimaryUpdate() {
  // TODO: Consider adding a method to perform this check as part of the `SecondaryProvider` in libaktualizr.
  // See https://gitlab.int.toradex.com/rd/torizon-core/aktualizr-torizon/-/merge_requests/7#note_70289

  EcuSerials serials;
  bpo::variables_map vm;
  Config config(vm);
  std::shared_ptr<INvStorage> storage;
  storage = INvStorage::newStorage(config.storage);
  boost::optional<Uptane::Target> pending;

  if (!secondary_provider_->getEcuSerialsForHwId(&serials) || serials.empty()) {
    throw std::runtime_error("Unable to get ECU serials from primary");
  }

  storage->loadInstalledVersions((serials[0].first).ToString(), nullptr, &pending);
  return !!pending;
}

bool DockerComposeSecondary::getFirmwareInfo(Uptane::InstalledImageInfo& firmware_info) const {
  std::string content;

  if (!boost::filesystem::exists(sconfig.firmware_path)) {
    firmware_info.name = std::string("noimage");
    content = "";
  } else {
    if (!boost::filesystem::exists(sconfig.target_name_path)) {
      firmware_info.name = std::string("docker-compose.yml");
    } else {
      firmware_info.name = Utils::readFile(sconfig.target_name_path.string());
    }

    // Read compose-file and transform it into its original form in memory.
    DockerComposeFile dcfile;
    if (!dcfile.read(sconfig.firmware_path)) {
      LOG_WARNING << "Could not read compose " << sconfig.firmware_path;
      return false;
    }
    dcfile.backwardTransform();
    content = dcfile.toString();
  }

  firmware_info.hash = Uptane::ManifestIssuer::generateVersionHashStr(content);
  firmware_info.len = content.size();

  LOG_TRACE << "DockerComposeSecondary::getFirmwareInfo: hash=" << firmware_info.hash;

  return true;
}

// TODO: Consider a more general mechanism to allow all secondaries to complete a previous installation.
// See https://gitlab.int.toradex.com/rd/torizon-core/aktualizr-torizon/-/merge_requests/7#note_70289
// TODO: Consider implementing `completePendingInstall()` instead; consider also returning a different
// result code to ask for a reboot OR giving a delay for the reboot: `shutdown +1`.
void DockerComposeSecondary::validateInstall() {
  std::string compose_file = sconfig.firmware_path.string();
  std::string compose_file_new = compose_file + ".tmp";
  ComposeManager pending_check(compose_file, compose_file_new);

  Uptane::EcuSerial serial = getSerial();
  std::shared_ptr<INvStorage> storage;
  bpo::variables_map vm;
  Config config(vm);
  storage = INvStorage::newStorage(config.storage);
  boost::optional<Uptane::Target> pending_target;
  storage->loadInstalledVersions(serial.ToString(), nullptr, &pending_target);
  if (!pending_target && !access(compose_file_new.c_str(), F_OK)) {
    LOG_INFO << "Incomplete update detected.";
    pending_check.containers_stopped = true;
    pending_check.rollback();
  }

  if (!pending_check.pendingUpdate()) {
    LOG_ERROR << "Unable to complete pending container update";

    // TODO: Consider providing a method for clearing the pending flag via the `SecondaryProvider` in libaktualizr.
    // See https://gitlab.int.toradex.com/rd/torizon-core/aktualizr-torizon/-/merge_requests/7#note_70289
    // Pending compose update failed, unset pending flag so that the rest of the Uptane process can go forward again
    storage->saveEcuInstallationResult(serial, data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, ""));
    storage->saveInstalledVersion(serial.ToString(), *pending_target, InstalledVersionUpdateMode::kNone);

    pending_check.rollback();
  }
}

}  // namespace Primary
