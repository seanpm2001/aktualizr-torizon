// TODO: [TDX] This module is used by the secondary only but is in the primary directory.
#include <boost/filesystem/path.hpp>

#include "compose_manager.h"
#include "logging/logging.h"
#include "libaktualizr/config.h"

namespace bpo = boost::program_options;

ComposeManager::ComposeManager(const std::string &compose_file_current, const std::string &compose_file_new) {
  compose_file_current_ = compose_file_current;
  compose_file_new_ = compose_file_new;
  compose_cmd_  = compose_program_ + " --file ";
}

bool ComposeManager::pull(const std::string &compose_file) {
  LOG_INFO << "Running docker-compose pull";
  return cmd.run(compose_cmd_ + compose_file + " pull --no-parallel");
}

bool ComposeManager::up(const std::string &compose_file) {
  LOG_INFO << "Running docker-compose up";
  return cmd.run(compose_cmd_ + compose_file + " -p torizon up --detach --remove-orphans");
}

bool ComposeManager::down(const std::string &compose_file) {
  LOG_INFO << "Running docker-compose down";
  return cmd.run(compose_cmd_ + compose_file + " -p torizon down");
}

bool ComposeManager::cleanup() {
  LOG_INFO << "Removing not used containers, networks and images";
  return cmd.run("docker system prune -a --force");
}

bool ComposeManager::completeUpdate() {
  if (!access(compose_file_current_.c_str(), F_OK)) {
    if (down(compose_file_current_) == false) {
      LOG_ERROR << "Error running docker-compose down";
      return false;
    }
    containers_stopped = true;
  }

  if (up(compose_file_new_) == false) {
    LOG_ERROR << "Error running docker-compose up";
    return false;
  }

  rename(compose_file_new_.c_str(), compose_file_current_.c_str());

  cleanup();

  return true;
}

bool ComposeManager::checkRollback() {
  LOG_INFO << "Checking rollback status";
  std::vector<std::string> output = cmd.runResult(printenv_program_);

  if (std::find_if(output.begin(), output.end(), [](const std::string& str) { return str.find("rollback=1") != std::string::npos; }) != output.end()) {
    return true;
  }
  else {
    return false;
  }
}

bool ComposeManager::update(bool offline, bool sync) {
  LOG_INFO << "Updating containers via docker-compose";

  sync_update = sync;
  reboot = false;

  if (sync_update) {
    LOG_INFO << "OSTree update pending. This is a synchronous update transaction.";
  }

  containers_stopped = false;

  if (!offline) {
    // Only try to pull images upon an online update.
    if (pull(compose_file_new_) == false) {
      LOG_ERROR << "Error running docker-compose pull";
      return false;
    }
  }

  if (!sync_update) {
    if(completeUpdate() == false) {
      return false;
    }
  }

  return true;
}

bool ComposeManager::pendingUpdate() {
  if (!access(compose_file_new_.c_str(), F_OK)) {
    LOG_INFO << "Finishing pending container updates via docker-compose";
  }
  else {
    return true;
  }

  if (checkRollback()) {
    sync_update = false;
    reboot = false;
    return false;
  }
  else {
    sync_update = true;
    reboot = true;
  }

  containers_stopped = false;

  if(completeUpdate() == false) {
    return false;
  }

  return true;
}

bool ComposeManager::rollback() {

  LOG_INFO << "Rolling back container update";

  if (containers_stopped == true) {
    up(compose_file_current_);
    containers_stopped = false;
  }

  remove(compose_file_new_.c_str());

  cleanup();

  if (sync_update) {
    cmd.run("fw_setenv rollback 1");
  }

  if (reboot) {
    cmd.run("reboot");
  }

  return true;
}
