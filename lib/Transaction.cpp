/*
  The Transaction class is the central API class for the lifecycle of a
  transaction: It will open and close transactions and execute commands in the
  correct context.

  Copyright (c) 2016 - 2020 SUSE LLC

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "Transaction.h"
#include "Configuration.h"
#include "Log.h"
#include "Overlay.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>
using namespace std;

Transaction::Transaction() {
    tulog.debug("Constructor Transaction");
}

Transaction::Transaction(string uuid) {
    snapshot = SnapshotFactory::get();
    snapshot->open(uuid);
    mount();
}

Transaction::~Transaction() {
    tulog.debug("Destructor Transaction");
    dirsToMount.clear();
    try {
        filesystem::remove_all(filesystem::path{bindDir});
        if (isInitialized())
            snapshot->abort();
    }  catch (const exception &e) {
        tulog.error("ERROR: ", e.what());
    }
}

bool Transaction::isInitialized() {
    return snapshot ? true : false;
}

string Transaction::getSnapshot()
{
    return snapshot->getUid();
}

void Transaction::mount(string base) {
    dirsToMount.push_back(make_unique<BindMount>("/dev"));
    dirsToMount.push_back(make_unique<BindMount>("/var/log"));

    Mount mntVar{"/var"};
    if (mntVar.isMount()) {
        dirsToMount.push_back(make_unique<BindMount>("/var/cache"));
        dirsToMount.push_back(make_unique<BindMount>("/var/lib/alternatives"));
    }
    unique_ptr<Mount> mntEtc{new Mount{"/etc"}};
    if (mntEtc->isMount() && mntEtc->getFS() == "overlay") {
        Overlay overlay = Overlay{snapshot->getUid()};
        overlay.create(base);

        overlay.updateMountDirs(mntEtc, config.get("DRACUT_SYSROOT"));
        mntEtc->persist(snapshot->getRoot() / "etc" / "fstab");
        overlay.updateMountDirs(mntEtc);

        overlay.sync(snapshot->getRoot());

        dirsToMount.push_back(std::move(mntEtc));

        // Make sure both the snapshot and the overlay contain all relevant fstab data, i.e.
        // user modifications from the overlay are present in the root fs and the /etc
        // overlay is visible in the overlay
        filesystem::copy(filesystem::path{snapshot->getRoot() / "etc" / "fstab"}, overlay.upperdir, filesystem::copy_options::overwrite_existing);
    }

    unique_ptr<Mount> mntProc{new Mount{"/proc"}};
    mntProc->setType("proc");
    mntProc->setSource("none");
    dirsToMount.push_back(std::move(mntProc));

    unique_ptr<Mount> mntSys{new Mount{"/sys"}};
    mntSys->setType("sysfs");
    mntSys->setSource("sys");
    dirsToMount.push_back(std::move(mntSys));

    if (BindMount{"/root"}.isMount())
        dirsToMount.push_back(make_unique<BindMount>("/root"));

    if (BindMount{"/boot/writable"}.isMount())
        dirsToMount.push_back(make_unique<BindMount>("/boot/writable"));

    dirsToMount.push_back(make_unique<BindMount>("/.snapshots"));

    for (auto it = dirsToMount.begin(); it != dirsToMount.end(); ++it) {
        it->get()->mount(snapshot->getRoot());
    }

    // When all mounts are set up, then bind mount everything into a temporary
    // directory - GRUB needs to have an actual mount point for the root
    // partition
    char bindTemplate[] = "/tmp/transactional-update-XXXXXX";
    bindDir = mkdtemp(bindTemplate);
    unique_ptr<BindMount> mntBind{new BindMount{bindDir, MS_REC}};
    mntBind->setSource(snapshot->getRoot());
    mntBind->mount();
    dirsToMount.push_back(std::move(mntBind));
}

void Transaction::init(string base) {
    snapshot = SnapshotFactory::get();
    if (base == "active")
        base = snapshot->getCurrent();
    else if (base == "default")
        base =snapshot->getDefault();
    snapshot->create(base);

    mount(base);
}

int Transaction::execute(const char* argv[]) {
    std::string opts = "Executing `";
    int i = 0;
    while (argv[i]) {
        if (i > 0)
            opts.append(" ");
        opts.append(argv[i]);
        i++;
    }
    opts.append("`:");
    tulog.info(opts);

    int status = 1;
    pid_t pid = fork();
    if (pid < 0) {
        throw runtime_error{"fork() failed: " + string(strerror(errno))};
    } else if (pid == 0) {
        if (chroot(bindDir.c_str()) < 0) {
            throw runtime_error{"Chrooting to " + bindDir + " failed: " + string(strerror(errno))};
        }
        cout << "◸" << flush;
        if (execvp(argv[0], (char* const*)argv) < 0) {
            throw runtime_error{"Calling " + string(argv[0]) + " failed: " + string(strerror(errno))};
        }
    } else {
        int ret;
        ret = waitpid(pid, &status, 0);
        cout << "◿" << endl;
        if (ret < 0) {
            throw runtime_error{"waitpid() failed: " + string(strerror(errno))};
        } else {
            tulog.info("Application returned with exit status ", WEXITSTATUS(status), ".");
        }
    }
    return WEXITSTATUS(status);
}

void Transaction::finalize() {
    snapshot->close();

    std::unique_ptr<Snapshot> defaultSnap = SnapshotFactory::get();
    defaultSnap->open(snapshot->getDefault());
    if (defaultSnap->isReadOnly())
        snapshot->setReadOnly(true);

    snapshot.reset();
}

void Transaction::keep() {
    snapshot.reset();
}
