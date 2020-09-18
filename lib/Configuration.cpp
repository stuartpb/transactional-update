/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2020 SUSE LLC */

/*
  Retrieves configuration values, set via configuration file or using
  default values otherwise.
 */

#include "Configuration.h"
#include "Util.h"
#include <map>
#include <stdexcept>
#include <libeconf.h>

namespace TransactionalUpdate {

Configuration::Configuration() {
    econf_err error = econf_newIniFile(&key_file);
    if (error)
        throw std::runtime_error{"Could not create default configuration."};
    std::map<const char*, const char*> defaults = {
        {"DRACUT_SYSROOT", "/sysroot"},
        {"LOCKFILE", "/var/run/transactional-update.pid"},
        {"OVERLAY_DIR", "/var/lib/overlay"}
    };
    for(auto &[key, value] : defaults) {
        error = econf_setStringValue(key_file, "", key, value);
        if (error)
            throw std::runtime_error{"Could not set default value for '" + std::string(key) + "'."};
    }
}

Configuration::~Configuration() {
    econf_freeFile(key_file);
}

std::string Configuration::get(const std::string &key) {
    CString val;
    econf_err error = econf_getStringValue(key_file, "", key.c_str(), &val.ptr);
    if (error)
        throw std::runtime_error{"Could not read configuration setting '" + key + "'."};
    return std::string(val);
}

} // namespace TransactionalUpdate
