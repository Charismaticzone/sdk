/**
 * @file sync.cpp
 * @brief Class for synchronizing local and remote trees
 *
 * (c) 2013-2014 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#include <type_traits>
#include <unordered_set>

#include "mega.h"

#ifdef ENABLE_SYNC
#include "mega/sync.h"
#include "mega/megaapp.h"
#include "mega/transfer.h"
#include "mega/megaclient.h"
#include "mega/base64.h"

namespace mega {

const int Sync::SCANNING_DELAY_DS = 5;
const int Sync::EXTRA_SCANNING_DELAY_DS = 150;
const int Sync::FILE_UPDATE_DELAY_DS = 30;
const int Sync::FILE_UPDATE_MAX_DELAY_SECS = 60;
const dstime Sync::RECENT_VERSION_INTERVAL_SECS = 10800;

namespace {

// Need this to store `LightFileFingerprint` by-value in `FingerprintSet`
struct LightFileFingerprintComparator
{
    bool operator()(const LightFileFingerprint& lhs, const LightFileFingerprint& rhs) const
    {
        return LightFileFingerprintCmp{}(&lhs, &rhs);
    }
};

// Represents a file/folder for use in assigning fs IDs
struct FsFile
{
    handle fsid;
    LocalPath path;
};

// Caches fingerprints
class FingerprintCache
{
public:
    using FingerprintSet = std::set<LightFileFingerprint, LightFileFingerprintComparator>;

    // Adds a new fingerprint
    template<typename T, typename = typename std::enable_if<std::is_same<LightFileFingerprint, typename std::decay<T>::type>::value>::type>
    const LightFileFingerprint* add(T&& ffp)
    {
         const auto insertPair = mFingerprints.insert(std::forward<T>(ffp));
         return &*insertPair.first;
    }

    // Returns the set of all fingerprints
    const FingerprintSet& all() const
    {
        return mFingerprints;
    }

private:
    FingerprintSet mFingerprints;
};

using FingerprintLocalNodeMap = std::multimap<const LightFileFingerprint*, LocalNode*, LightFileFingerprintCmp>;
using FingerprintFileMap = std::multimap<const LightFileFingerprint*, FsFile, LightFileFingerprintCmp>;

// Collects all syncable filesystem paths in the given folder under `localpath`
set<LocalPath> collectAllPathsInFolder(Sync& sync, MegaApp& app, FileSystemAccess& fsaccess, LocalPath& localpath,
                                    LocalPath& localdebris)
{
    auto fa = fsaccess.newfileaccess(false);
    if (!fa->fopen(localpath, true, false))
    {
        LOG_err << "Unable to open path: " << localpath.toPath(fsaccess);
        return {};
    }
    if (fa->mIsSymLink)
    {
        LOG_debug << "Ignoring symlink: " << localpath.toPath(fsaccess);
        return {};
    }
    assert(fa->type == FOLDERNODE);

    auto da = std::unique_ptr<DirAccess>{fsaccess.newdiraccess()};
    if (!da->dopen(&localpath, fa.get(), false))
    {
        LOG_err << "Unable to open directory: " << localpath.toPath(fsaccess);
        return {};
    }

    set<LocalPath> paths; // has to be a std::set to enforce same sorting as `children` of `LocalNode`

    LocalPath localname;
    while (da->dnext(localpath, localname, false))
    {
        ScopedLengthRestore restoreLength(localpath);
        localpath.appendWithSeparator(localname, false);

        // check if this record is to be ignored
        const auto name = localname.toName(fsaccess, sync.mFilesystemType);
        if (app.sync_syncable(&sync, name.c_str(), localpath))
        {
            // skip the sync's debris folder
            if (!localdebris.isContainingPathOf(localpath))
            {
                paths.insert(localpath);
            }
        }
    }

    return paths;
}

// Combines another fingerprint into `ffp`
void hashCombineFingerprint(LightFileFingerprint& ffp, const LightFileFingerprint& other)
{
    hashCombine(ffp.size, other.size);
    hashCombine(ffp.mtime, other.mtime);
}

// Combines the fingerprints of all file nodes in the given map
bool combinedFingerprint(LightFileFingerprint& ffp, const localnode_map& nodeMap)
{
    bool success = false;
    for (const auto& nodePair : nodeMap)
    {
        const LocalNode& l = *nodePair.second;
        if (l.type == FILENODE)
        {
            LightFileFingerprint lFfp;
            lFfp.genfingerprint(l.size, l.mtime);
            hashCombineFingerprint(ffp, lFfp);
            success = true;
        }
    }
    return success;
}

// Combines the fingerprints of all files in the given paths
bool combinedFingerprint(LightFileFingerprint& ffp, FileSystemAccess& fsaccess, const set<LocalPath>& paths)
{
    bool success = false;
    for (auto& path : paths)
    {
        auto fa = fsaccess.newfileaccess(false);
        auto pathArg = path; // todo: sort out const
        if (!fa->fopen(pathArg, true, false))
        {
            LOG_err << "Unable to open path: " << path.toPath(fsaccess);
            success = false;
            break;
        }
        if (fa->mIsSymLink)
        {
            LOG_debug << "Ignoring symlink: " << path.toPath(fsaccess);
            continue;
        }
        if (fa->type == FILENODE)
        {
            LightFileFingerprint faFfp;
            faFfp.genfingerprint(fa->size, fa->mtime);
            hashCombineFingerprint(ffp, faFfp);
            success = true;
        }
    }
    return success;
}

// Computes the fingerprint of the given `l` (file or folder) and stores it in `ffp`
bool computeFingerprint(LightFileFingerprint& ffp, const LocalNode& l)
{
    if (l.type == FILENODE)
    {
        ffp.genfingerprint(l.size, l.mtime);
        return true;
    }
    else if (l.type == FOLDERNODE)
    {
        return combinedFingerprint(ffp, l.children);
    }
    else
    {
        assert(false && "Invalid node type");
        return false;
    }
}

// Computes the fingerprint of the given `fa` (file or folder) and stores it in `ffp`
bool computeFingerprint(LightFileFingerprint& ffp, FileSystemAccess& fsaccess,
                        FileAccess& fa, LocalPath& path, const set<LocalPath>& paths)
{
    if (fa.type == FILENODE)
    {
        assert(paths.empty());
        ffp.genfingerprint(fa.size, fa.mtime);
        return true;
    }
    else if (fa.type == FOLDERNODE)
    {
        return combinedFingerprint(ffp, fsaccess, paths);
    }
    else
    {
        assert(false && "Invalid node type");
        return false;
    }
}

// Collects all `LocalNode`s by storing them in `localnodes`, keyed by LightFileFingerprint.
// Invalidates the fs IDs of all local nodes.
// Stores all fingerprints in `fingerprints` for later reference.
void collectAllLocalNodes(FingerprintCache& fingerprints, FingerprintLocalNodeMap& localnodes,
                          LocalNode& l, handlelocalnode_map& fsidnodes)
{
    // invalidate fsid of `l`
    l.fsid = mega::UNDEF;
    if (l.fsid_it != fsidnodes.end())
    {
        fsidnodes.erase(l.fsid_it);
        l.fsid_it = fsidnodes.end();
    }
    // collect fingerprint
    LightFileFingerprint ffp;
    if (computeFingerprint(ffp, l))
    {
        const auto ffpPtr = fingerprints.add(std::move(ffp));
        localnodes.insert(std::make_pair(ffpPtr, &l));
    }
    if (l.type == FILENODE)
    {
        return;
    }
    for (auto& childPair : l.children)
    {
        collectAllLocalNodes(fingerprints, localnodes, *childPair.second, fsidnodes);
    }
}

// Collects all `File`s by storing them in `files`, keyed by FileFingerprint.
// Stores all fingerprints in `fingerprints` for later reference.
void collectAllFiles(bool& success, FingerprintCache& fingerprints, FingerprintFileMap& files,
                     Sync& sync, MegaApp& app, FileSystemAccess& fsaccess, LocalPath& localpath,
                     LocalPath& localdebris)
{
    auto insertFingerprint = [&files, &fingerprints](FileSystemAccess& fsaccess, FileAccess& fa,
                                                     LocalPath& path, const set<LocalPath>& paths)
    {
        LightFileFingerprint ffp;
        if (computeFingerprint(ffp, fsaccess, fa, path, paths))
        {
            const auto ffpPtr = fingerprints.add(std::move(ffp));
            files.insert(std::make_pair(ffpPtr, FsFile{fa.fsid, path}));
        }
    };

    auto fa = fsaccess.newfileaccess(false);
    if (!fa->fopen(localpath, true, false))
    {
        LOG_err << "Unable to open path: " << localpath.toPath(fsaccess);
        success = false;
        return;
    }
    if (fa->mIsSymLink)
    {
        LOG_debug << "Ignoring symlink: " << localpath.toPath(fsaccess);
        return;
    }
    if (!fa->fsidvalid)
    {
        LOG_err << "Invalid fs id for: " << localpath.toPath(fsaccess);
        success = false;
        return;
    }

    if (fa->type == FILENODE)
    {
        insertFingerprint(fsaccess, *fa, localpath, {});
    }
    else if (fa->type == FOLDERNODE)
    {
        const auto paths = collectAllPathsInFolder(sync, app, fsaccess, localpath, localdebris);
        insertFingerprint(fsaccess, *fa, localpath, paths);
        fa.reset();
        for (const auto& path : paths)
        {
            LocalPath tmpPath = path;
            collectAllFiles(success, fingerprints, files, sync, app, fsaccess, tmpPath, localdebris);
        }
    }
    else
    {
        assert(false && "Invalid file type");
        success = false;
        return;
    }
}

// Assigns fs IDs from `files` to those `localnodes` that match the fingerprints found in `files`.
// If there are multiple matches we apply a best-path heuristic.
size_t assignFilesystemIdsImpl(const FingerprintCache& fingerprints, FingerprintLocalNodeMap& localnodes,
                               FingerprintFileMap& files, handlelocalnode_map& fsidnodes, FileSystemAccess& fsaccess)
{
    LocalPath nodePath;
    size_t assignmentCount = 0;
    for (const auto& fp : fingerprints.all())
    {
        const auto nodeRange = localnodes.equal_range(&fp);
        const auto nodeCount = std::distance(nodeRange.first, nodeRange.second);
        if (nodeCount <= 0)
        {
            continue;
        }

        const auto fileRange = files.equal_range(&fp);
        const auto fileCount = std::distance(fileRange.first, fileRange.second);
        if (fileCount <= 0)
        {
            // without files we cannot assign fs IDs to these localnodes, so no need to keep them
            localnodes.erase(nodeRange.first, nodeRange.second);
            continue;
        }

        struct Element
        {
            int score;
            handle fsid;
            LocalNode* l;
        };
        std::vector<Element> elements;
        elements.reserve(nodeCount * fileCount);

        for (auto nodeIt = nodeRange.first; nodeIt != nodeRange.second; ++nodeIt)
        {
            auto l = nodeIt->second;
            if (l != l->sync->localroot.get()) // never assign fs ID to the root localnode
            {
                nodePath = l->getLocalPath();
                for (auto fileIt = fileRange.first; fileIt != fileRange.second; ++fileIt)
                {
                    auto& filePath = fileIt->second.path;
                    const auto score = computeReversePathMatchScore(nodePath, filePath, fsaccess);
                    if (score > 0) // leaf name must match
                    {
                        elements.push_back({score, fileIt->second.fsid, l});
                    }
                }
            }
        }

        // Sort in descending order by score. Elements with highest score come first
        std::sort(elements.begin(), elements.end(), [](const Element& e1, const Element& e2)
                                                    {
                                                        return e1.score > e2.score;
                                                    });

        std::unordered_set<handle> usedFsIds;
        for (const auto& e : elements)
        {
            if (e.l->fsid == mega::UNDEF // node not assigned
                && usedFsIds.find(e.fsid) == usedFsIds.end()) // fsid not used
            {
                e.l->setfsid(e.fsid, fsidnodes);
                usedFsIds.insert(e.fsid);
                ++assignmentCount;
            }
        }

        // the fingerprint that these files and localnodes correspond to has now finished processing
        files.erase(fileRange.first, fileRange.second);
        localnodes.erase(nodeRange.first, nodeRange.second);
    }
    return assignmentCount;
}

} // anonymous

int computeReversePathMatchScore(const LocalPath& path1, const LocalPath& path2, const FileSystemAccess& fsaccess)
{
    if (path1.empty() || path2.empty())
    {
        return 0;
    }

    const auto path1End = path1.localpath.size() - 1;
    const auto path2End = path2.localpath.size() - 1;

    size_t index = 0;
    size_t separatorBias = 0;
    LocalPath accumulated;
    while (index <= path1End && index <= path2End)
    {
        const auto value1 = path1.localpath[path1End - index];
        const auto value2 = path2.localpath[path2End - index];
        if (value1 != value2)
        {
            break;
        }
        accumulated.localpath.push_back(value1);

        ++index;

        if (!accumulated.localpath.empty())
        {
            if (accumulated.localpath.back() == LocalPath::localPathSeparator)
            {
                ++separatorBias;
                accumulated.clear();
            }
        }
    }

    if (index > path1End && index > path2End) // we got to the beginning of both paths (full score)
    {
        return static_cast<int>(index - separatorBias);
    }
    else // the paths only partly match
    {
        return static_cast<int>(index - separatorBias - accumulated.localpath.size());
    }
}

bool assignFilesystemIds(Sync& sync, MegaApp& app, FileSystemAccess& fsaccess, handlelocalnode_map& fsidnodes,
                         LocalPath& localdebris)
{
    auto& rootpath = sync.localroot->localname;
    LOG_info << "Assigning fs IDs at rootpath: " << rootpath.toPath(fsaccess);

    auto fa = fsaccess.newfileaccess(false);
    if (!fa->fopen(rootpath, true, false))
    {
        LOG_err << "Unable to open rootpath";
        return false;
    }
    if (fa->type != FOLDERNODE)
    {
        LOG_err << "rootpath not a folder";
        assert(false);
        return false;
    }
    if (fa->mIsSymLink)
    {
        LOG_err << "rootpath is a symlink";
        assert(false);
        return false;
    }
    fa.reset();

    bool success = true;

    FingerprintCache fingerprints;

    FingerprintLocalNodeMap localnodes;
    collectAllLocalNodes(fingerprints, localnodes, *sync.localroot, fsidnodes);
    LOG_info << "Number of localnodes: " << localnodes.size();

    if (localnodes.empty())
    {
        return success;
    }

    FingerprintFileMap files;
    collectAllFiles(success, fingerprints, files, sync, app, fsaccess, rootpath, localdebris);
    LOG_info << "Number of files: " << files.size();

    LOG_info << "Number of fingerprints: " << fingerprints.all().size();
    const auto assignmentCount = assignFilesystemIdsImpl(fingerprints, localnodes, files, fsidnodes, fsaccess);
    LOG_info << "Number of fsid assignments: " << assignmentCount;

    return success;
}

SyncConfigBag::SyncConfigBag(DbAccess& dbaccess, FileSystemAccess& fsaccess, PrnGen& rng, const std::string& id)
{
    std::string dbname = "syncconfigsv2_" + id;
    mTable.reset(dbaccess.open(rng, &fsaccess, &dbname, false, false));
    if (!mTable)
    {
        LOG_err << "Unable to open DB table: " << dbname;
        assert(false);
        return;
    }

    mTable->rewind();

    uint32_t tableId;
    std::string data;
    while (mTable->next(&tableId, &data))
    {
        auto syncConfig = SyncConfig::unserialize(data);
        if (!syncConfig)
        {
            LOG_err << "Unable to unserialize sync config at id: " << tableId;
            assert(false);
            continue;
        }
        syncConfig->dbid = tableId;

        mSyncConfigs.insert(std::make_pair(syncConfig->getTag(), *syncConfig));
        if (tableId > mTable->nextid)
        {
            mTable->nextid = tableId;
        }
    }
    ++mTable->nextid;
}

void SyncConfigBag::insert(const SyncConfig& syncConfig)
{
    auto insertOrUpdate = [this](const uint32_t id, const SyncConfig& syncConfig)
    {
        std::string data;
        const_cast<SyncConfig&>(syncConfig).serialize(&data);
        DBTableTransactionCommitter committer{mTable.get()};
        if (!mTable->put(id, &data)) // put either inserts or updates
        {
            LOG_err << "Incomplete database put at id: " << mTable->nextid;
            assert(false);
            mTable->abort();
            return false;
        }
        return true;
    };

    map<int, SyncConfig>::iterator syncConfigIt = mSyncConfigs.find(syncConfig.getTag());
    if (syncConfigIt == mSyncConfigs.end()) // syncConfig is new
    {
        if (mTable)
        {
            if (!insertOrUpdate(mTable->nextid, syncConfig))
            {
                return;
            }
        }
        auto insertPair = mSyncConfigs.insert(std::make_pair(syncConfig.getTag(), syncConfig));
        if (mTable)
        {
            insertPair.first->second.dbid = mTable->nextid;
            ++mTable->nextid;
        }
    }
    else // syncConfig exists already
    {
        const uint32_t tableId = syncConfigIt->second.dbid;
        if (mTable)
        {
            if (!insertOrUpdate(tableId, syncConfig))
            {
                return;
            }
        }
        syncConfigIt->second = syncConfig;
        syncConfigIt->second.dbid = tableId;
    }
}

bool SyncConfigBag::removeByTag(const int tag)
{
    auto syncConfigPair = mSyncConfigs.find(tag);
    if (syncConfigPair != mSyncConfigs.end())
    {
        if (mTable)
        {
            DBTableTransactionCommitter committer{mTable.get()};
            if (!mTable->del(syncConfigPair->second.dbid))
            {
                LOG_err << "Incomplete database del at id: " << syncConfigPair->second.dbid;
                assert(false);
                mTable->abort();
            }
        }
        mSyncConfigs.erase(syncConfigPair);
        return true;
    }
    return false;
}

const SyncConfig* SyncConfigBag::get(const int tag) const
{
    auto syncConfigPair = mSyncConfigs.find(tag);
    if (syncConfigPair != mSyncConfigs.end())
    {
        return &syncConfigPair->second;
    }
    return nullptr;
}


const SyncConfig* SyncConfigBag::getByNodeHandle(handle nodeHandle) const
{
    for (const auto& syncConfigPair : mSyncConfigs)
    {
        if (syncConfigPair.second.getRemoteNode() == nodeHandle)
            return &syncConfigPair.second;
    }
    return nullptr;
}

void SyncConfigBag::clear()
{
    if (mTable)
    {
        mTable->truncate();
        mTable->nextid = 0;
    }
    mSyncConfigs.clear();
}

std::vector<SyncConfig> SyncConfigBag::all() const
{
    std::vector<SyncConfig> syncConfigs;
    for (const auto& syncConfigPair : mSyncConfigs)
    {
        syncConfigs.push_back(syncConfigPair.second);
    }
    return syncConfigs;
}

// new Syncs are automatically inserted into the session's syncs list
// and a full read of the subtree is initiated
Sync::Sync(MegaClient* cclient, SyncConfig &config, const char* cdebris,
           LocalPath* clocaldebris, Node* remotenode, bool cinshare, int ctag, void *cappdata)
: localroot(new LocalNode)
{
    isnetwork = false;
    client = cclient;
    tag = ctag;
    inshare = cinshare;
    appData = cappdata;
    errorCode = NO_SYNC_ERROR;
    tmpfa = NULL;
    initializing = true;
    updatedfilesize = ~0;
    updatedfilets = 0;
    updatedfileinitialts = 0;

    localbytes = 0;
    localnodes[FILENODE] = 0;
    localnodes[FOLDERNODE] = 0;

    state = SYNC_INITIALSCAN;
    statecachetable = NULL;

    fullscan = true;
    scanseqno = 0;

    mLocalPath = config.getLocalPath();
    LocalPath crootpath = LocalPath::fromPath(mLocalPath, *client->fsaccess);

    mBackupState = config.isBackup() ? SYNC_BACKUP_MIRROR
                                     : SYNC_BACKUP_NONE;

    if (cdebris)
    {
        debris = cdebris;
        localdebris = LocalPath::fromPath(debris, *client->fsaccess);

        dirnotify.reset(client->fsaccess->newdirnotify(crootpath, localdebris, client->waiter));

        localdebris.prependWithSeparator(crootpath);
    }
    else
    {
        localdebris = *clocaldebris;

        // FIXME: pass last segment of localdebris
        dirnotify.reset(client->fsaccess->newdirnotify(crootpath, localdebris, client->waiter));
    }
    dirnotify->sync = this;

    // set specified fsfp or get from fs if none
    const auto cfsfp = config.getLocalFingerprint();
    if (cfsfp)
    {
        fsfp = cfsfp;
    }
    else
    {
        fsfp = dirnotify->fsfingerprint();
        config.setLocalFingerprint(fsfp);
    }

    fsstableids = dirnotify->fsstableids();
    LOG_info << "Filesystem IDs are stable: " << fsstableids;

    mFilesystemType = client->fsaccess->getlocalfstype(crootpath);

    localroot->init(this, FOLDERNODE, NULL, crootpath, nullptr);  // the root node must have the absolute path.  We don't store shortname, to avoid accidentally using relative paths.
    localroot->setnode(remotenode);

#ifdef __APPLE__
    if (macOSmajorVersion() >= 19) //macOS catalina+
    {
        LOG_debug << "macOS 10.15+ filesystem detected. Checking fseventspath.";
        string supercrootpath = "/System/Volumes/Data" + crootpath.platformEncoded();

        int fd = open(supercrootpath.c_str(), O_RDONLY);
        if (fd == -1)
        {
            LOG_debug << "Unable to open path using fseventspath.";
            mFsEventsPath = crootpath.platformEncoded();
        }
        else
        {
            char buf[MAXPATHLEN];
            if (fcntl(fd, F_GETPATH, buf) < 0)
            {
                LOG_debug << "Using standard paths to detect filesystem notifications.";
                mFsEventsPath = crootpath.platformEncoded();
            }
            else
            {
                LOG_debug << "Using fsevents paths to detect filesystem notifications.";
                mFsEventsPath = supercrootpath;
            }
            close(fd);
        }
    }
#endif

    sync_it = client->syncs.insert(client->syncs.end(), this);

    if (client->dbaccess)
    {
        // open state cache table
        handle tableid[3];
        string dbname;

        auto fas = client->fsaccess->newfileaccess(false);

        if (fas->fopen(crootpath, true, false))
        {
            tableid[0] = fas->fsid;
            tableid[1] = remotenode->nodehandle;
            tableid[2] = client->me;

            dbname.resize(sizeof tableid * 4 / 3 + 3);
            dbname.resize(Base64::btoa((byte*)tableid, sizeof tableid, (char*)dbname.c_str()));

            statecachetable = client->dbaccess->open(client->rng, client->fsaccess, &dbname, false, false);

            readstatecache();
        }
    }
}

Sync::~Sync()
{
    // must be set to prevent remote mass deletion while rootlocal destructor runs
    assert(state == SYNC_CANCELED || state == SYNC_FAILED || state == SYNC_DISABLED);
    mDestructorRunning = true;

    // unlock tmp lock
    tmpfa.reset();

    // stop all active and pending downloads
    if (localroot->node)
    {
        TreeProcDelSyncGet tdsg;
        // Create a committer to ensure we update the transfer database in an efficient single commit,
        // if there are transactions in progress.
        DBTableTransactionCommitter committer(client->tctable);
        client->proctree(localroot->node, &tdsg);
    }

    delete statecachetable;

    client->syncs.erase(sync_it);
    client->syncactivity = true;

    {
        // Create a committer and recursively delete all the associated LocalNodes, and their associated transfer and file objects.
        // If any have transactions in progress, the committer will ensure we update the transfer database in an efficient single commit.
        DBTableTransactionCommitter committer(client->tctable);
        localroot.reset();
    }
}

bool Sync::backupModified()
{
    changestate(SYNC_DISABLED, BACKUP_MODIFIED);
    return false;
}

bool Sync::isBackup() const
{
    return mBackupState != SYNC_BACKUP_NONE;
}

bool Sync::isMirroring() const
{
    return mBackupState == SYNC_BACKUP_MIRROR;
}

bool Sync::isMonitoring() const
{
    return mBackupState == SYNC_BACKUP_MONITOR;
}

void Sync::monitor()
{
    assert(mBackupState == SYNC_BACKUP_MIRROR);

    mBackupState = SYNC_BACKUP_MONITOR;
}

void Sync::addstatecachechildren(uint32_t parent_dbid, idlocalnode_map* tmap, LocalPath& localpath, LocalNode *p, int maxdepth)
{
    auto range = tmap->equal_range(parent_dbid);

    for (auto it = range.first; it != range.second; it++)
    {
        ScopedLengthRestore restoreLen(localpath);

        localpath.appendWithSeparator(it->second->localname, true);

        LocalNode* l = it->second;
        Node* node = l->node;
        handle fsid = l->fsid;
        m_off_t size = l->size;

        // clear localname to force newnode = true in setnameparent
        l->localname.clear();

        // if we already have the shortname from database, use that, otherwise (db is from old code) look it up
        std::unique_ptr<LocalPath> shortname;
        if (l->slocalname_in_db)
        {
            // null if there is no shortname, or the shortname matches the localname.
            shortname.reset(l->slocalname.release());
        }
        else
        {
            shortname = client->fsaccess->fsShortname(localpath);
        }

        l->init(this, l->type, p, localpath, std::move(shortname));

#ifdef DEBUG
        auto fa = client->fsaccess->newfileaccess(false);
        if (fa->fopen(localpath))  // exists, is file
        {
            auto sn = client->fsaccess->fsShortname(localpath);
            assert(!l->localname.empty() &&
                ((!l->slocalname && (!sn || l->localname == *sn)) ||
                (l->slocalname && sn && !l->slocalname->empty() && *l->slocalname != l->localname && *l->slocalname == *sn)));
        }
#endif

        l->parent_dbid = parent_dbid;
        l->size = size;
        l->setfsid(fsid, client->fsidnode);
        l->setnode(node);

        if (!l->slocalname_in_db)
        {
            statecacheadd(l);
            if (insertq.size() > 50000)
            {
                cachenodes();  // periodically output updated nodes with shortname updates, so people who restart megasync still make progress towards a fast startup
            }
        }

        if (maxdepth)
        {
            addstatecachechildren(l->dbid, tmap, localpath, l, maxdepth - 1);
        }
    }
}

bool Sync::readstatecache()
{
    if (statecachetable && state == SYNC_INITIALSCAN)
    {
        string cachedata;
        idlocalnode_map tmap;
        uint32_t cid;
        LocalNode* l;

        statecachetable->rewind();

        // bulk-load cached nodes into tmap
        while (statecachetable->next(&cid, &cachedata, &client->key))
        {
            if ((l = LocalNode::unserialize(this, &cachedata)))
            {
                l->dbid = cid;
                tmap.insert(pair<int32_t,LocalNode*>(l->parent_dbid,l));
            }
        }

        // recursively build LocalNode tree, set scanseqnos to sync's current scanseqno
        addstatecachechildren(0, &tmap, localroot->localname, localroot.get(), 100);
        cachenodes();

        // trigger a single-pass full scan to identify deleted nodes
        fullscan = true;
        scanseqno++;

        return true;
    }

    return false;
}

const SyncConfig& Sync::getConfig() const
{
    assert(client->syncConfigs && "Calling getConfig() requires sync configs");
    const auto config = client->syncConfigs->get(tag);
    assert(config);
    return *config;
}

// remove LocalNode from DB cache
void Sync::statecachedel(LocalNode* l)
{
    if (state == SYNC_CANCELED)
    {
        return;
    }

    insertq.erase(l);

    if (l->dbid)
    {
        deleteq.insert(l->dbid);
    }
}

// insert LocalNode into DB cache
void Sync::statecacheadd(LocalNode* l)
{
    if (state == SYNC_CANCELED)
    {
        return;
    }

    if (l->dbid)
    {
        deleteq.erase(l->dbid);
    }

    insertq.insert(l);
}

void Sync::cachenodes()
{
    if (statecachetable && (state == SYNC_ACTIVE || (state == SYNC_INITIALSCAN && insertq.size() > 100)) && (deleteq.size() || insertq.size()))
    {
        LOG_debug << "Saving LocalNode database with " << insertq.size() << " additions and " << deleteq.size() << " deletions";
        statecachetable->begin();

        // deletions
        for (set<uint32_t>::iterator it = deleteq.begin(); it != deleteq.end(); it++)
        {
            statecachetable->del(*it);
        }

        deleteq.clear();

        // additions - we iterate until completion or until we get stuck
        bool added;

        do {
            added = false;

            for (set<LocalNode*>::iterator it = insertq.begin(); it != insertq.end(); )
            {
                if ((*it)->parent->dbid || (*it)->parent == localroot.get())
                {
                    statecachetable->put(MegaClient::CACHEDLOCALNODE, *it, &client->key);
                    insertq.erase(it++);
                    added = true;
                }
                else it++;
            }
        } while (added);

        statecachetable->commit();

        if (insertq.size())
        {
            LOG_err << "LocalNode caching did not complete";
        }
    }
}

void Sync::changestate(syncstate_t newstate, SyncError newSyncError)
{
    if (newstate != state || newSyncError != errorCode)
    {
        LOG_debug << "Sync state/error changing. from " << state << "/" << errorCode << " to "  << newstate << "/" << newSyncError;
        if (newstate != SYNC_CANCELED)
        {
            client->changeSyncState(tag, newstate, newSyncError);
        }

        state = newstate;
        errorCode = newSyncError;
        fullscan = false;
    }
}

// walk path and return corresponding LocalNode and its parent
// path must be relative to l or start with the root prefix if l == NULL
// path must be a full sync path, i.e. start with localroot->localname
// NULL: no match, optionally returns residual path
LocalNode* Sync::localnodebypath(LocalNode* l, const LocalPath& localpath, LocalNode** parent, LocalPath* outpath)
{
    assert(!outpath || outpath->empty());

    size_t subpathIndex = 0;

    if (!l)
    {
        // verify matching localroot prefix - this should always succeed for
        // internal use
        if (!localroot->localname.isContainingPathOf(localpath, &subpathIndex))
        {
            if (parent)
            {
                *parent = NULL;
            }

            return NULL;
        }

        l = localroot.get();
    }


    LocalPath component;

    while (localpath.nextPathComponent(subpathIndex, component))
    {
        if (parent)
        {
            *parent = l;
        }

        localnode_map::iterator it;
        if ((it = l->children.find(&component)) == l->children.end()
            && (it = l->schildren.find(&component)) == l->schildren.end())
        {
            // no full match: store residual path, return NULL with the
            // matching component LocalNode in parent
            if (outpath)
            {
                *outpath = std::move(component);
                auto remainder = localpath.subpathFrom(subpathIndex);
                if (!remainder.empty())
                {
                    outpath->appendWithSeparator(remainder, false);
                }
            }

            return NULL;
        }

        l = it->second;
    }

    // full match: no residual path, return corresponding LocalNode
    if (outpath)
    {
        outpath->clear();
    }
    return l;
}

bool Sync::assignfsids()
{
    return assignFilesystemIds(*this, *client->app, *client->fsaccess, client->fsidnode,
                               localdebris);
}

// scan localpath, add or update child nodes, call recursively for folder nodes
// localpath must be prefixed with Sync
bool Sync::scan(LocalPath* localpath, FileAccess* fa)
{
    if (fa)
    {
        assert(fa->type == FOLDERNODE);
    }
    if (!localdebris.isContainingPathOf(*localpath))
    {
        DirAccess* da;
        LocalPath localname;
        string name;
        bool success;

        if (SimpleLogger::logCurrentLevel >= logDebug)
        {
            LOG_debug << "Scanning folder: " << localpath->toPath(*client->fsaccess);
        }

        da = client->fsaccess->newdiraccess();

        // scan the dir, mark all items with a unique identifier
        if ((success = da->dopen(localpath, fa, false)))
        {
            while (da->dnext(*localpath, localname, client->followsymlinks))
            {
                name = localname.toName(*client->fsaccess, mFilesystemType);

                ScopedLengthRestore restoreLen(*localpath);
                localpath->appendWithSeparator(localname, false);

                // check if this record is to be ignored
                if (client->app->sync_syncable(this, name.c_str(), *localpath))
                {
                    // skip the sync's debris folder
                    if (!localdebris.isContainingPathOf(*localpath))
                    {
                        LocalNode *l = NULL;
                        if (initializing)
                        {
                            // preload all cached LocalNodes
                            l = checkpath(NULL, localpath, nullptr, nullptr, false, da);
                        }

                        if (!l || l == (LocalNode*)~0)
                        {
                            // new record: place in notification queue
                            dirnotify->notify(DirNotify::DIREVENTS, NULL, LocalPath(*localpath));
                        }
                    }
                }
                else
                {
                    LOG_debug << "Excluded: " << name;
                }
            }
        }

        delete da;

        return success;
    }
    else return false;
}

// check local path - if !localname, localpath is relative to l, with l == NULL
// being the root of the sync
// if localname is set, localpath is absolute and localname its last component
// path references a new FOLDERNODE: returns created node
// path references a existing FILENODE: returns node
// otherwise, returns NULL
LocalNode* Sync::checkpath(LocalNode* l, LocalPath* input_localpath, string* const localname, dstime *backoffds, bool wejustcreatedthisfolder, DirAccess* iteratingDir)
{
    LocalNode* ll = l;
    bool newnode = false, changed = false;
    bool isroot;

    LocalNode* parent;
    string path;           // UTF-8 representation of tmppath
    LocalPath tmppath;     // full path represented by l + localpath
    LocalPath newname;     // portion of tmppath not covered by the existing
                           // LocalNode structure (always the last path component
                           // that does not have a corresponding LocalNode yet)

    if (localname)
    {
        // shortcut case (from within syncdown())
        isroot = false;
        parent = l;
        l = NULL;

        path = input_localpath->toPath(*client->fsaccess);
        assert(path.size());
    }
    else
    {
        // construct full filesystem path in tmppath
        if (l)
        {
            tmppath = l->getLocalPath();
        }

        if (!input_localpath->empty())
        {
            tmppath.appendWithSeparator(*input_localpath, false);
        }

        // look up deepest existing LocalNode by path, store remainder (if any)
        // in newname
        LocalNode *tmp = localnodebypath(l, *input_localpath, &parent, &newname);
        size_t index = 0;

        if (newname.findNextSeparator(index))
        {
            LOG_warn << "Parent not detected yet. Unknown remainder: " << newname.toPath(*client->fsaccess);
            if (parent)
            {
                LocalPath notifyPath = parent->getLocalPath();
                notifyPath.appendWithSeparator(newname.subpathTo(index), true);
                dirnotify->notify(DirNotify::DIREVENTS, l, std::move(notifyPath), true);
            }
            return NULL;
        }

        l = tmp;

        path = tmppath.toPath(*client->fsaccess);

        // path invalid?
        if ( ( !l && newname.empty() ) || !path.size())
        {
            LOG_warn << "Invalid path: " << path;
            return NULL;
        }

        string name = !newname.empty() ? newname.toName(*client->fsaccess, mFilesystemType) : l->name;

        if (!client->app->sync_syncable(this, name.c_str(), tmppath))
        {
            LOG_debug << "Excluded: " << path;
            return NULL;
        }

        isroot = l == localroot.get() && newname.empty();
    }

    LOG_verbose << "Scanning: " << path << " in=" << initializing << " full=" << fullscan << " l=" << l;
    LocalPath* localpathNew = localname ? input_localpath : &tmppath;

    if (parent)
    {
        if (state != SYNC_INITIALSCAN && !parent->node)
        {
            LOG_warn << "Parent doesn't exist yet: " << path;
            return (LocalNode*)~0;
        }
    }

    // attempt to open/type this file
    auto fa = client->fsaccess->newfileaccess(false);

    if (initializing || fullscan)
    {
        // find corresponding LocalNode by file-/foldername
        size_t lastpart = localpathNew->getLeafnameByteIndex(*client->fsaccess);

        LocalPath fname(localpathNew->subpathFrom(lastpart));

        LocalNode* cl = (parent ? parent : localroot.get())->childbyname(&fname);
        if (initializing && cl)
        {
            // the file seems to be still in the folder
            // mark as present to prevent deletions if the file is not accesible
            // in that case, the file would be checked again after the initialization
            cl->deleted = false;
            cl->setnotseen(0);
            l->scanseqno = scanseqno;
        }

        // match cached LocalNode state during initial/rescan to prevent costly re-fingerprinting
        // (just compare the fsids, sizes and mtimes to detect changes)
        if (fa->fopen(*localpathNew, false, false, iteratingDir))
        {
            if (cl && fa->fsidvalid && fa->fsid == cl->fsid)
            {
                // node found and same file
                l = cl;
                l->deleted = false;
                l->setnotseen(0);

                // if it's a file, size and mtime must match to qualify
                if (l->type != FILENODE || (l->size == fa->size && l->mtime == fa->mtime))
                {
                    LOG_verbose << "Cached localnode is still valid. Type: " << l->type << "  Size: " << l->size << "  Mtime: " << l->mtime;
                    l->scanseqno = scanseqno;

                    if (l->type == FOLDERNODE)
                    {
                        scan(localpathNew, fa.get());
                    }
                    else
                    {
                        localbytes += l->size;
                    }

                    return l;
                }
            }
        }
        else
        {
            LOG_warn << "Error opening file during the initialization: " << path;
        }

        if (initializing)
        {
            if (cl)
            {
                LOG_verbose << "Outdated localnode. Type: " << cl->type << "  Size: " << cl->size << "  Mtime: " << cl->mtime
                            << "    FaType: " << fa->type << "  FaSize: " << fa->size << "  FaMtime: " << fa->mtime;
            }
            else
            {
                LOG_verbose << "New file. FaType: " << fa->type << "  FaSize: " << fa->size << "  FaMtime: " << fa->mtime;
            }
            return NULL;
        }

        fa = client->fsaccess->newfileaccess(false);
    }

    if (fa->fopen(*localpathNew, true, false))
    {
        if (!isroot)
        {
            if (l)
            {
                if (l->type == fa->type)
                {
                    // mark as present
                    l->setnotseen(0);

                    if (fa->type == FILENODE)
                    {
                        // has the file been overwritten or changed since the last scan?
                        // or did the size or mtime change?
                        if (fa->fsidvalid)
                        {
                            // if fsid has changed, the file was overwritten
                            // (FIXME: handle type changes)
                            if (l->fsid != fa->fsid)
                            {
                                handlelocalnode_map::iterator it;
#ifdef _WIN32
                                const char *colon;
#endif
                                fsfp_t fp1, fp2;

                                // was the file overwritten by moving an existing file over it?
                                if ((it = client->fsidnode.find(fa->fsid)) != client->fsidnode.end()
                                        && (l->sync == it->second->sync
                                            || ((fp1 = l->sync->dirnotify->fsfingerprint())
                                                && (fp2 = it->second->sync->dirnotify->fsfingerprint())
                                                && (fp1 == fp2)
                                            #ifdef _WIN32
                                                // only consider fsid matches between different syncs for local drives with the
                                                // same drive letter, to prevent problems with cloned Volume IDs
                                                && (colon = strstr(parent->sync->localroot->name.c_str(), ":"))
                                                && !memcmp(parent->sync->localroot->name.c_str(),
                                                       it->second->sync->localroot->name.c_str(),
                                                       colon - parent->sync->localroot->name.c_str())
                                            #endif
                                                )
                                            )
                                    )
                                {
                                    // catch the not so unlikely case of a false fsid match due to
                                    // e.g. a file deletion/creation cycle that reuses the same inode
                                    if (it->second->mtime != fa->mtime || it->second->size != fa->size)
                                    {
                                        l->mtime = -1;  // trigger change detection
                                        delete it->second;   // delete old LocalNode
                                    }
                                    else
                                    {
                                        LOG_debug << "File move/overwrite detected";

                                        // delete existing LocalNode...
                                        delete l;

                                        // ...move remote node out of the way...
                                        client->execsyncdeletions();

                                        // ...and atomically replace with moved one
                                        client->app->syncupdate_local_move(this, it->second, path.c_str());

                                        // (in case of a move, this synchronously updates l->parent and l->node->parent)
                                        it->second->setnameparent(parent, localpathNew, client->fsaccess->fsShortname(*localpathNew));

                                        // mark as seen / undo possible deletion
                                        it->second->setnotseen(0);

                                        statecacheadd(it->second);

                                        return it->second;
                                    }
                                }
                                else
                                {
                                    l->mtime = -1;  // trigger change detection
                                }
                            }
                        }

                        // no fsid change detected or overwrite with unknown file:
                        if (fa->mtime != l->mtime || fa->size != l->size)
                        {
                            if (fa->fsidvalid && l->fsid != fa->fsid)
                            {
                                l->setfsid(fa->fsid, client->fsidnode);
                            }

                            m_off_t dsize = l->size > 0 ? l->size : 0;

                            if (l->genfingerprint(fa.get()) && l->size >= 0)
                            {
                                localbytes -= dsize - l->size;
                            }

                            client->app->syncupdate_local_file_change(this, l, path.c_str());

                            DBTableTransactionCommitter committer(client->tctable);
                            client->stopxfer(l, &committer); // TODO:  can we use one committer for all the files in the folder?  Or for the whole recursion?
                            l->bumpnagleds();
                            l->deleted = false;

                            client->syncactivity = true;

                            statecacheadd(l);

                            fa.reset();

                            if (isnetwork && l->type == FILENODE)
                            {
                                LOG_debug << "Queueing extra fs notification for modified file";
                                dirnotify->notify(DirNotify::EXTRA, NULL, LocalPath(*localpathNew));
                            }
                            return l;
                        }
                    }
                    else
                    {
                        // (we tolerate overwritten folders, because we do a
                        // content scan anyway)
                        if (fa->fsidvalid && fa->fsid != l->fsid)
                        {
                            l->setfsid(fa->fsid, client->fsidnode);
                            newnode = true;
                        }
                    }
                }
                else
                {
                    LOG_debug << "node type changed: recreate";
                    delete l;
                    l = NULL;
                }
            }

            // new node
            if (!l)
            {
                // rename or move of existing node?
                handlelocalnode_map::iterator it;
#ifdef _WIN32
                const char *colon;
#endif
                fsfp_t fp1, fp2;
                if (fa->fsidvalid && (it = client->fsidnode.find(fa->fsid)) != client->fsidnode.end()
                    // additional checks to prevent wrong fsid matches
                    && it->second->type == fa->type
                    && (!parent
                        || (it->second->sync == parent->sync)
                        || ((fp1 = it->second->sync->dirnotify->fsfingerprint())
                            && (fp2 = parent->sync->dirnotify->fsfingerprint())
                            && (fp1 == fp2)
                        #ifdef _WIN32
                            // allow moves between different syncs only for local drives with the
                            // same drive letter, to prevent problems with cloned Volume IDs
                            && (colon = strstr(parent->sync->localroot->name.c_str(), ":"))
                            && !memcmp(parent->sync->localroot->name.c_str(),
                                   it->second->sync->localroot->name.c_str(),
                                   colon - parent->sync->localroot->name.c_str())
                        #endif
                            )
                       )
                    && ((it->second->type != FILENODE && !wejustcreatedthisfolder)
                        || (it->second->mtime == fa->mtime && it->second->size == fa->size)))
                {
                    LOG_debug << client->clientname << "Move detected by fsid in checkpath. Type: " << it->second->type << " new path: " << path << " old localnode: " << it->second->localnodedisplaypath(*client->fsaccess);

                    if (fa->type == FILENODE && backoffds)
                    {
                        // logic to detect files being updated in the local computer moving the original file
                        // to another location as a temporary backup

                        m_time_t currentsecs = m_time();
                        if (!updatedfileinitialts)
                        {
                            updatedfileinitialts = currentsecs;
                        }

                        if (currentsecs >= updatedfileinitialts)
                        {
                            if (currentsecs - updatedfileinitialts <= FILE_UPDATE_MAX_DELAY_SECS)
                            {
                                bool waitforupdate = false;
                                auto local = it->second->getLocalPath();
                                auto prevfa = client->fsaccess->newfileaccess(false);

                                bool exists = prevfa->fopen(local);
                                if (exists)
                                {
                                    LOG_debug << "File detected in the origin of a move";

                                    if (currentsecs >= updatedfilets)
                                    {
                                        if ((currentsecs - updatedfilets) < (FILE_UPDATE_DELAY_DS / 10))
                                        {
                                            LOG_verbose << "currentsecs = " << currentsecs << "  lastcheck = " << updatedfilets
                                                      << "  currentsize = " << prevfa->size << "  lastsize = " << updatedfilesize;
                                            LOG_debug << "The file was checked too recently. Waiting...";
                                            waitforupdate = true;
                                        }
                                        else if (updatedfilesize != prevfa->size)
                                        {
                                            LOG_verbose << "currentsecs = " << currentsecs << "  lastcheck = " << updatedfilets
                                                      << "  currentsize = " << prevfa->size << "  lastsize = " << updatedfilesize;
                                            LOG_debug << "The file size has changed since the last check. Waiting...";
                                            updatedfilesize = prevfa->size;
                                            updatedfilets = currentsecs;
                                            waitforupdate = true;
                                        }
                                        else
                                        {
                                            LOG_debug << "The file size seems stable";
                                        }
                                    }
                                    else
                                    {
                                        LOG_warn << "File checked in the future";
                                    }

                                    if (!waitforupdate)
                                    {
                                        if (currentsecs >= prevfa->mtime)
                                        {
                                            if (currentsecs - prevfa->mtime < (FILE_UPDATE_DELAY_DS / 10))
                                            {
                                                LOG_verbose << "currentsecs = " << currentsecs << "  mtime = " << prevfa->mtime;
                                                LOG_debug << "File modified too recently. Waiting...";
                                                waitforupdate = true;
                                            }
                                            else
                                            {
                                                LOG_debug << "The modification time seems stable.";
                                            }
                                        }
                                        else
                                        {
                                            LOG_warn << "File modified in the future";
                                        }
                                    }
                                }
                                else
                                {
                                    if (prevfa->retry)
                                    {
                                        LOG_debug << "The file in the origin is temporarily blocked. Waiting...";
                                        waitforupdate = true;
                                    }
                                    else
                                    {
                                        LOG_debug << "There isn't anything in the origin path";
                                    }
                                }

                                if (waitforupdate)
                                {
                                    LOG_debug << "Possible file update detected.";
                                    *backoffds = FILE_UPDATE_DELAY_DS;
                                    return NULL;
                                }
                            }
                            else
                            {
                                int creqtag = client->reqtag;
                                client->reqtag = 0;
                                client->sendevent(99438, "Timeout waiting for file update");
                                client->reqtag = creqtag;
                            }
                        }
                        else
                        {
                            LOG_warn << "File check started in the future";
                        }
                    }

                    client->app->syncupdate_local_move(this, it->second, path.c_str());

                    // (in case of a move, this synchronously updates l->parent
                    // and l->node->parent)
                    it->second->setnameparent(parent, localpathNew, client->fsaccess->fsShortname(*localpathNew));

                    // make sure that active PUTs receive their updated filenames
                    client->updateputs();

                    statecacheadd(it->second);

                    // unmark possible deletion
                    it->second->setnotseen(0);

                    // immediately scan folder to detect deviations from cached state
                    if (fullscan && fa->type == FOLDERNODE)
                    {
                        scan(localpathNew, fa.get());
                    }
                }
                else if (fa->mIsSymLink)
                {
                    LOG_debug << "checked path is a symlink.  Parent: " << (parent ? parent->name : "NO");
                    //doing nothing for the moment
                }
                else
                {
                    // this is a new node: add
                    LOG_debug << "New localnode.  Parent: " << (parent ? parent->name : "NO");
                    l = new LocalNode;
                    l->init(this, fa->type, parent, *localpathNew, client->fsaccess->fsShortname(*localpathNew));

                    if (fa->fsidvalid)
                    {
                        l->setfsid(fa->fsid, client->fsidnode);
                    }

                    newnode = true;
                }
            }
        }

        if (l)
        {
            // detect file changes or recurse into new subfolders
            if (l->type == FOLDERNODE)
            {
                if (newnode)
                {
                    scan(localpathNew, fa.get());
                    client->app->syncupdate_local_folder_addition(this, l, path.c_str());

                    if (!isroot)
                    {
                        statecacheadd(l);
                    }
                }
                else
                {
                    l = NULL;
                }
            }
            else
            {
                if (isroot)
                {
                    // root node cannot be a file
                    LOG_err << "The local root node is a file";
                    changestate(SYNC_FAILED, INVALID_LOCAL_TYPE);
                }
                else
                {
                    if (fa->fsidvalid && l->fsid != fa->fsid)
                    {
                        l->setfsid(fa->fsid, client->fsidnode);
                    }

                    if (l->size > 0)
                    {
                        localbytes -= l->size;
                    }

                    if (l->genfingerprint(fa.get()))
                    {
                        changed = true;
                        l->bumpnagleds();
                        l->deleted = false;
                    }

                    if (l->size > 0)
                    {
                        localbytes += l->size;
                    }

                    if (newnode)
                    {
                        client->app->syncupdate_local_file_addition(this, l, path.c_str());
                    }
                    else if (changed)
                    {
                        client->app->syncupdate_local_file_change(this, l, path.c_str());
                        DBTableTransactionCommitter committer(client->tctable); // TODO:  can we use one committer for all the files in the folder?  Or for the whole recursion?
                        client->stopxfer(l, &committer);
                    }

                    if (newnode || changed)
                    {
                        statecacheadd(l);
                    }
                }
            }
        }

        if (changed || newnode)
        {
            if (isnetwork && l->type == FILENODE)
            {
                LOG_debug << "Queueing extra fs notification for new file";
                dirnotify->notify(DirNotify::EXTRA, NULL, LocalPath(*localpathNew));
            }

            client->syncactivity = true;
        }
    }
    else
    {
        LOG_warn << "Error opening file";
        if (fa->retry)
        {
            // fopen() signals that the failure is potentially transient - do
            // nothing and request a recheck
            LOG_warn << "File blocked. Adding notification to the retry queue: " << path;
            dirnotify->notify(DirNotify::RETRY, ll, LocalPath(*localpathNew));
            client->syncfslockretry = true;
            client->syncfslockretrybt.backoff(SCANNING_DELAY_DS);
            client->blockedfile = *localpathNew;
        }
        else if (l)
        {
            // immediately stop outgoing transfer, if any
            if (l->transfer)
            {
                DBTableTransactionCommitter committer(client->tctable); // TODO:  can we use one committer for all the files in the folder?  Or for the whole recursion?
                client->stopxfer(l, &committer);
            }

            client->syncactivity = true;

            // in fullscan mode, missing files are handled in bulk in deletemissing()
            // rather than through setnotseen()
            if (!fullscan)
            {
                l->setnotseen(1);
            }
        }

        l = NULL;
    }

    return l;
}

bool Sync::checkValidNotification(int q, Notification& notification)
{
    // This code moved from filtering before going on notifyq, to filtering after when it's thread-safe to do so

    if (q == DirNotify::DIREVENTS || q == DirNotify::EXTRA)
    {
        Notification next;
        while (dirnotify->notifyq[q].peekFront(next)
            && next.localnode == notification.localnode && next.path == notification.path)
        {
            dirnotify->notifyq[q].popFront(next);  // this is the only thread removing from the queue so it will be the same item
            if (!notification.timestamp || !next.timestamp)
            {
                notification.timestamp = 0;  // immediate
            }
            else
            {
                notification.timestamp = std::max(notification.timestamp, next.timestamp);
            }
            LOG_debug << "Next notification repeats, skipping duplicate";
        }
    }

    if (notification.timestamp && !initializing && q == DirNotify::DIREVENTS)
    {
        LocalPath tmppath;
        if (notification.localnode)
        {
            tmppath = notification.localnode->getLocalPath();
        }

        if (!notification.path.empty())
        {
            tmppath.appendWithSeparator(notification.path, false);
        }

        attr_map::iterator ait;
        auto fa = client->fsaccess->newfileaccess(false);
        bool success = fa->fopen(tmppath, false, false);
        LocalNode *ll = localnodebypath(notification.localnode, notification.path);
        if ((!ll && !success && !fa->retry) // deleted file
            || (ll && success && ll->node && ll->node->localnode == ll
                && (ll->type != FILENODE || (*(FileFingerprint *)ll) == (*(FileFingerprint *)ll->node))
                && (ait = ll->node->attrs.map.find('n')) != ll->node->attrs.map.end()
                && ait->second == ll->name
                && fa->fsidvalid && fa->fsid == ll->fsid && fa->type == ll->type
                && (ll->type != FILENODE || (ll->mtime == fa->mtime && ll->size == fa->size))))
        {
            LOG_debug << "Self filesystem notification skipped";
            return false;
        }
    }
    return true;
}

// add or refresh local filesystem item from scan stack, add items to scan stack
// returns 0 if a parent node is missing, ~0 if control should be yielded, or the time
// until a retry should be made (500 ms minimum latency).
dstime Sync::procscanq(int q)
{
    dstime dsmin = Waiter::ds - SCANNING_DELAY_DS;
    LocalNode* l;

    Notification notification;
    while (dirnotify->notifyq[q].popFront(notification))
    {
        if (!checkValidNotification(q, notification))
        {
            continue;
        }

        LOG_verbose << "Scanning... Remaining files: " << dirnotify->notifyq[q].size();

        if (notification.timestamp > dsmin)
        {
            LOG_verbose << "Scanning postponed. Modification too recent";
            dirnotify->notifyq[q].unpopFront(notification);
            return notification.timestamp - dsmin;
        }

        if ((l = notification.localnode) != (LocalNode*)~0)
        {
            dstime backoffds = 0;
            LOG_verbose << "Checkpath: " << notification.path.toPath(*client->fsaccess);

            l = checkpath(l, &notification.path, NULL, &backoffds, false, nullptr);
            if (backoffds)
            {
                LOG_verbose << "Scanning deferred during " << backoffds << " ds";
                notification.timestamp = Waiter::ds + backoffds - SCANNING_DELAY_DS;
                dirnotify->notifyq[q].unpopFront(notification);
                return backoffds;
            }
            updatedfilesize = ~0;
            updatedfilets = 0;
            updatedfileinitialts = 0;

            // defer processing because of a missing parent node?
            if (l == (LocalNode*)~0)
            {
                LOG_verbose << "Scanning deferred";
                dirnotify->notifyq[q].unpopFront(notification);
                return 0;
            }
        }
        else
        {
            string utf8path = notification.path.toPath(*client->fsaccess);
            LOG_debug << "Notification skipped: " << utf8path;
        }

        // we return control to the application in case a filenode was added
        // (in order to avoid lengthy blocking episodes due to multiple
        // consecutive fingerprint calculations)
        // or if new nodes are being added due to a copy/delete operation
        if ((l && l != (LocalNode*)~0 && l->type == FILENODE) || client->syncadding)
        {
            break;
        }
    }

    if (dirnotify->notifyq[q].empty())
    {
        if (q == DirNotify::DIREVENTS)
        {
            client->syncactivity = true;
        }
    }
    else if (dirnotify->notifyq[!q].empty())
    {
        cachenodes();
    }

    return dstime(~0);
}

// delete all child LocalNodes that have been missing for two consecutive scans (*l must still exist)
void Sync::deletemissing(LocalNode* l)
{
    LocalPath path;
    std::unique_ptr<FileAccess> fa;
    for (localnode_map::iterator it = l->children.begin(); it != l->children.end(); )
    {
        if (scanseqno-it->second->scanseqno > 1)
        {
            if (!fa)
            {
                fa = client->fsaccess->newfileaccess();
            }
            client->unlinkifexists(it->second, fa.get(), path);
            delete it++->second;
        }
        else
        {
            deletemissing(it->second);
            it++;
        }
    }
}

bool Sync::movetolocaldebris(LocalPath& localpath)
{
    char buf[32];
    struct tm tms;
    string day, localday;
    bool havedir = false;
    struct tm* ptm = m_localtime(m_time(), &tms);

    for (int i = -3; i < 100; i++)
    {
        ScopedLengthRestore restoreLen(localdebris);

        if (i == -2 || i > 95)
        {
            LOG_verbose << "Creating local debris folder";
            client->fsaccess->mkdirlocal(localdebris, true);
        }

        sprintf(buf, "%04d-%02d-%02d", ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);

        if (i >= 0)
        {
            sprintf(strchr(buf, 0), " %02d.%02d.%02d.%02d", ptm->tm_hour,  ptm->tm_min, ptm->tm_sec, i);
        }

        day = buf;
        localdebris.appendWithSeparator(LocalPath::fromPath(day, *client->fsaccess), true);

        if (i > -3)
        {
            LOG_verbose << "Creating daily local debris folder";
            havedir = client->fsaccess->mkdirlocal(localdebris, false) || client->fsaccess->target_exists;
        }

        localdebris.appendWithSeparator(localpath.subpathFrom(localpath.getLeafnameByteIndex(*client->fsaccess)), true);

        client->fsaccess->skip_errorreport = i == -3;  // we expect a problem on the first one when the debris folders or debris day folders don't exist yet
        if (client->fsaccess->renamelocal(localpath, localdebris, false))
        {
            client->fsaccess->skip_errorreport = false;
            return true;
        }
        client->fsaccess->skip_errorreport = false;

        if (client->fsaccess->transient_error)
        {
            return false;
        }

        if (havedir && !client->fsaccess->target_exists)
        {
            return false;
        }
    }

    return false;
}

XBackupConfig::XBackupConfig()
  : drivePath()
  , sourcePath()
  , heartbeatID(UNDEF)
  , targetHandle(UNDEF)
  , lastError(NO_SYNC_ERROR)
  , tag(0)
  , enabled(false)
{
}

bool XBackupConfig::valid() const
{
    return !(drivePath.empty()
             || sourcePath.empty()
             || targetHandle == UNDEF);
}

bool XBackupConfig::operator==(const XBackupConfig& rhs) const
{
    return drivePath == rhs.drivePath
           && sourcePath == rhs.sourcePath
           && heartbeatID == rhs.heartbeatID
           && targetHandle == rhs.targetHandle
           && lastError == rhs.lastError
           && tag == rhs.tag
           && enabled == rhs.enabled;
}

bool XBackupConfig::operator!=(const XBackupConfig& rhs) const
{
    return !(*this == rhs);
}

XBackupConfig translate(const MegaClient& client, const SyncConfig &config)
{
    XBackupConfig result;

    assert(config.drivePath().size());
    assert(config.isBackup());
    assert(config.isExternal());

    result.enabled = config.getEnabled();
    result.heartbeatID = config.getBackupId();
    result.lastError = config.getError();
    result.tag = config.getTag();
    result.targetHandle = config.getRemoteNode();

    const auto& drivePath = config.drivePath();
    const auto sourcePath = config.getLocalPath().substr(drivePath.size());

    const auto& fsAccess = *client.fsaccess;

    result.drivePath = LocalPath::fromPath(drivePath, fsAccess);
    result.sourcePath = LocalPath::fromPath(sourcePath, fsAccess);

    return result;
}

SyncConfig translate(const MegaClient& client, const XBackupConfig& config)
{
    string drivePath = config.drivePath.toPath(*client.fsaccess);
    string sourcePath;
    string targetPath;

    // Source Path
    {
        LocalPath temp = config.drivePath;

        temp.appendWithSeparator(config.sourcePath, false);

        sourcePath = temp.toPath(*client.fsaccess);
    }

    // Target Path
    if (config.targetHandle != UNDEF)
    {
        const auto* targetNode =
          client.nodebyhandle(config.targetHandle);

        assert(targetNode);

        targetPath = targetNode->displaypath();
    }

    // Build config.
    auto result =
      SyncConfig(config.tag,
                 sourcePath,
                 sourcePath,
                 config.targetHandle,
                 std::move(targetPath),
                 0,
                 string_vector(),
                 config.enabled,
                 SyncConfig::TYPE_BACKUP,
                 true,
                 false,
                 config.lastError,
                 config.heartbeatID);

    result.drivePath(std::move(drivePath));

    return result;
}

const unsigned int XBackupConfigDB::NUM_SLOTS = 2;

XBackupConfigDB::XBackupConfigDB(const LocalPath& drivePath,
                                 XBackupConfigDBObserver& observer)
  : mDrivePath(drivePath)
  , mObserver(observer)
  , mTagToConfig()
  , mTargetToConfig()
  , mSlot(0)
{
}

XBackupConfigDB::~XBackupConfigDB()
{
    // Drop the configs.
    clear(false);
}

const XBackupConfig* XBackupConfigDB::add(const XBackupConfig& config)
{
    // Add (or update) the config and flush.
    return add(config, true);
}

void XBackupConfigDB::clear()
{
    // Drop the configs and flush.
    clear(true);
}

const XBackupConfigMap& XBackupConfigDB::configs() const
{
    return mTagToConfig;
}

const LocalPath& XBackupConfigDB::drivePath() const
{
    return mDrivePath;
}

const XBackupConfig* XBackupConfigDB::get(const int tag) const
{
    auto it = mTagToConfig.find(tag);

    if (it != mTagToConfig.end())
    {
        return &it->second;
    }

    return nullptr;
}

const XBackupConfig* XBackupConfigDB::get(const handle targetHandle) const
{
    auto it = mTargetToConfig.find(targetHandle);

    if (it != mTargetToConfig.end())
    {
        return it->second;
    }

    return nullptr;
}

error XBackupConfigDB::read(XBackupConfigIOContext& ioContext)
{
    vector<unsigned int> slots;

    // Determine which slots we should load first, if any.
    if (ioContext.get(mDrivePath, slots) != API_OK)
    {
        // Couldn't get a list of slots.
        return API_ENOENT;
    }

    // Try and load the database from one of the slots.
    for (const auto& slot : slots)
    {
        // Can we read the database from this slot?
        if (read(ioContext, slot) == API_OK)
        {
            // Update the slot number.
            mSlot = (slot + 1) % NUM_SLOTS;

            // Yep, loaded.
            return API_OK;
        }
    }

    // Couldn't load the database.
    return API_EREAD;
}

error XBackupConfigDB::remove(const int tag)
{
    // Remove the config, if present and flush.
    return remove(tag, true);
}

error XBackupConfigDB::remove(const handle targetHandle)
{
    // Any config present with the given target handle?
    if (const auto* config = get(targetHandle))
    {
        // Yep, remove it.
        return remove(config->tag, true);
    }

    // Nope.
    return API_ENOENT;
}

error XBackupConfigDB::write(XBackupConfigIOContext& ioContext)
{
    JSONWriter writer;

    // Serialize the database.
    ioContext.serialize(mTagToConfig, writer);

    // Try and write the database out to disk.
    if (ioContext.write(mDrivePath, writer.getstring(), mSlot) != API_OK)
    {
        // Couldn't write the database out to disk.
        return API_EWRITE;
    }

    // Rotate the slot.
    mSlot = (mSlot + 1) % NUM_SLOTS;

    return API_OK;
}

const XBackupConfig* XBackupConfigDB::add(const XBackupConfig& config,
                                          const bool flush)
{
    // Sanity check.
    assert(config.drivePath == mDrivePath);

    auto it = mTagToConfig.find(config.tag);

    // Do we already have a config with this tag?
    if (it != mTagToConfig.end())
    {
        // Has the config changed?
        if (config == it->second)
        {
            // Hasn't changed.
            return &it->second;
        }

        // Tell the observer a config's changed.
        mObserver.onChange(*this, it->second, config);

        // Tell the observer we need to be written.
        if (flush)
        {
            // But only if this change should be flushed.
            mObserver.onDirty(*this);
        }

        // Remove the existing config from the target index.
        mTargetToConfig.erase(it->second.targetHandle);

        // Update the config.
        it->second = config;

        // Sanity check.
        assert(mTargetToConfig.count(config.targetHandle) == 0);

        // Index the updated config by target, if possible.
        if (config.targetHandle != UNDEF)
        {
            mTargetToConfig.emplace(config.targetHandle, &it->second);
        }

        // We're done.
        return &it->second;
    }

    // Add the config to the database.
    auto result = mTagToConfig.emplace(config.tag, config);

    // Sanity check.
    assert(mTargetToConfig.count(config.targetHandle) == 0);

    // Index the new config by target, if possible.
    if (config.targetHandle != UNDEF)
    {
        mTargetToConfig.emplace(config.targetHandle, &result.first->second);
    }

    // Tell the observer we've added a config.
    mObserver.onAdd(*this, config);

    // Tell the observer we need to be written.
    if (flush)
    {
        // But only if this change should be flushed.
        mObserver.onDirty(*this);
    }
    
    // We're done.
    return &result.first->second;
}

void XBackupConfigDB::clear(const bool flush)
{
    // Are there any configs to remove?
    if (mTagToConfig.empty())
    {
        // Nope.
        return;
    }

    // Tell the observer we've removed the configs.
    for (auto& it : mTagToConfig)
    {
        mObserver.onRemove(*this, it.second);
    }

    // Tell the observer we need to be written.
    if (flush)
    {
        // But only if these changes should be flushed.
        mObserver.onDirty(*this);
    }

    // Clear the backup target handle index.
    mTargetToConfig.clear();

    // Clear the config database.
    mTagToConfig.clear();
}

error XBackupConfigDB::read(XBackupConfigIOContext& ioContext,
                            const unsigned int slot)
{
    // Try and read the database from the specified slot.
    string data;

    if (ioContext.read(mDrivePath, data, slot) != API_OK)
    {
        // Couldn't read the database.
        return API_EREAD;
    }

    // Try and deserialize the configs contained in the database.
    XBackupConfigMap configs;
    JSON reader(data);

    if (!ioContext.deserialize(configs, reader))
    {
        // Couldn't deserialize the configs.
        return API_EREAD;
    }

    // Remove configs that aren't present on disk.
    for (const auto& it : mTagToConfig)
    {
        if (!configs.count(it.first))
        {
            remove(it.first, false);
        }
    }

    // Add (or update) configs.
    for (auto& it : configs)
    {
        // Correct config's drive path.
        it.second.drivePath = mDrivePath;

        // Add / update the config.
        add(it.second, false);
    }

    return API_OK;
}

error XBackupConfigDB::remove(const int tag, const bool flush)
{
    auto it = mTagToConfig.find(tag);

    // Any config present with the given tag?
    if (it == mTagToConfig.end())
    {
        // Nope.
        return API_ENOENT;
    }

    // Tell the observer we've removed a config.
    mObserver.onRemove(*this, it->second);

    // Tell the observer we need to be written.
    if (flush)
    {
        // But only if this change should be flushed.
        mObserver.onDirty(*this);
    }

    // Remove the config from the target handle index.
    mTargetToConfig.erase(it->second.targetHandle);

    // Remove the config from the database.
    mTagToConfig.erase(it);

    // We're done.
    return API_OK;
}

XBackupConfigIOContext::XBackupConfigIOContext(SymmCipher& cipher,
                                               FileSystemAccess& fsAccess,
                                               const string& key,
                                               const string& name,
                                               PrnGen& rng)
  : mCipher()
  , mFsAccess(fsAccess)
  , mName(LocalPath::fromPath(name, mFsAccess))
  , mRNG(rng)
  , mSigner()
{
    // Used to alter initial state for key derivation.
    static const byte auth[] = "authentication";
    static const byte codec[] = "codec";

    // These attributes *must* be sane.
    assert(!key.empty());
    assert(!name.empty());

    // Deserialize the key.
    string k = Base64::atob(key);
    assert(k.size() == SymmCipher::KEYLENGTH);

    // Decrypt the key.
    cipher.ecb_decrypt(reinterpret_cast<byte*>(&k[0]), k.size());

    // Generate authentication and encryption keys.
    HKDF_HMAC_SHA512 hkdf;
    byte ka[SymmCipher::KEYLENGTH];
    byte ke[SymmCipher::KEYLENGTH];

    // Derive our authentication key.
    bool result =
      hkdf.deriveKey(ka,
                     sizeof(ka),
                     reinterpret_cast<const byte*>(&k[0]),
                     k.size(),
                     nullptr,
                     0,
                     auth,
                     sizeof(auth));
    assert(result);

    // Derive our encryption key.
    result =
      hkdf.deriveKey(ke,
                     sizeof(ke),
                     reinterpret_cast<const byte*>(&k[0]),
                     k.size(),
                     nullptr,
                     0,
                     codec,
                     sizeof(codec));
    assert(result);

    // Load the authenticaton key into our internal signer.
    mSigner.setkey(ka, sizeof(ka));

    // Load the encryption key into our internal cipher.
    mCipher.setkey(ke);
}

XBackupConfigIOContext::~XBackupConfigIOContext()
{
}

bool XBackupConfigIOContext::deserialize(XBackupConfigMap& configs,
                                         JSON& reader) const
{
    if (!reader.enterarray())
    {
        return false;
    }

    // Deserialize the configs.
    while (reader.enterobject())
    {
        XBackupConfig config;

        if (!deserialize(config, reader))
        {
            return false;
        }

        // So move is well-defined.
        const auto tag = config.tag;

        configs.emplace(tag, std::move(config));
    }

    return reader.leavearray();
}

error XBackupConfigIOContext::get(const LocalPath& drivePath,
                                  vector<unsigned int>& slots)
{
    using std::isdigit;
    using std::sort;

    using SlotTimePair = pair<unsigned int, m_time_t>;

    LocalPath globPath = drivePath;

    // Glob for configuration directory.
    globPath.appendWithSeparator(
      LocalPath::fromPath(BACKUP_CONFIG_DIR, mFsAccess), false);

    globPath.appendWithSeparator(mName, false);
    globPath.append(LocalPath::fromPath(".?", mFsAccess));

    // Open directory for iteration.
    unique_ptr<DirAccess> dirAccess(mFsAccess.newdiraccess());

    if (!dirAccess->dopen(&globPath, nullptr, true))
    {
        // Couldn't open directory for iteration.
        return API_ENOENT;
    }

    auto fileAcces = mFsAccess.newfileaccess(false);
    LocalPath filePath;
    vector<SlotTimePair> slotTimes;
    nodetype_t type;

    // Iterate directory.
    while (dirAccess->dnext(globPath, filePath, false, &type))
    {
        // Skip directories.
        if (type != FILENODE)
        {
            continue;
        }

        // Determine slot suffix.
        const char suffix = filePath.toPath(mFsAccess).back();

        // Skip invalid suffixes.
        if (!isdigit(suffix))
        {
            continue;
        }

        // Determine file's modification time.
        if (!fileAcces->fopen(filePath))
        {
            // Couldn't stat file.
            continue;
        }

        // Record this slot-time pair.
        slotTimes.emplace_back(suffix - 0x30, fileAcces->mtime);
    }

    // Sort the list of slot-time pairs.
    sort(slotTimes.begin(),
         slotTimes.end(),
         [](const auto& lhs, const auto& rhs)
         {
             // Order by descending modification time.
             if (lhs.second != rhs.second)
             {
                 return lhs.second > rhs.second;
             }

             // Otherwise by descending slot.
             return lhs.first > rhs.first;
         });

    // Transmit sorted list of slots to the caller.
    for (const auto& slotTime : slotTimes)
    {
        slots.emplace_back(slotTime.first);
    }

    return API_OK;
}

error XBackupConfigIOContext::read(const LocalPath& drivePath,
                                   string& data,
                                   const unsigned int slot)
{
    using std::to_string;

    LocalPath path = drivePath;

    // Generate path to the configuration file.
    path.appendWithSeparator(
      LocalPath::fromPath(BACKUP_CONFIG_DIR, mFsAccess), false);

    path.appendWithSeparator(mName, false);
    path.append(LocalPath::fromPath("." + to_string(slot), mFsAccess));

    // Try and open the file for reading.
    auto fileAccess = mFsAccess.newfileaccess(false);

    if (!fileAccess->fopen(path, true, false))
    {
        // Couldn't open the file for reading.
        return API_EREAD;
    }

    // Try and read the data from the file.
    string d;

    if (!fileAccess->fread(&d, fileAccess->size, 0, 0x0))
    {
        // Couldn't read the file.
        return API_EREAD;
    }

    // Try and decrypt the data.
    if (!decrypt(d, data))
    {
        // Couldn't decrypt the data.
        return API_EREAD;
    }

    return API_OK;
}

void XBackupConfigIOContext::serialize(const XBackupConfigMap& configs,
                                       JSONWriter& writer) const
{
    writer.beginarray();

    for (const auto& it : configs)
    {
        serialize(it.second, writer);
    }

    writer.endarray();
}

error XBackupConfigIOContext::write(const LocalPath& drivePath,
                                    const string& data,
                                    const unsigned int slot)
{
    using std::to_string;

    LocalPath path = drivePath;

    path.appendWithSeparator(
      LocalPath::fromPath(BACKUP_CONFIG_DIR, mFsAccess), false);

    // Try and create the backup configuration directory.
    if (!(mFsAccess.mkdirlocal(path) || mFsAccess.target_exists))
    {
        // Couldn't create the directory and it doesn't exist.
        return API_EFAILED;
    }

    // Generate the rest of the path.
    path.appendWithSeparator(mName, false);
    path.append(LocalPath::fromPath("." + to_string(slot), mFsAccess));

    // Open the file for writing.
    auto fileAccess = mFsAccess.newfileaccess(false);

    if (!fileAccess->fopen(path, false, true))
    {
        // Couldn't open the file for writing.
        return API_EWRITE;
    }

    // Truncate the file only if necessary.
    if (fileAccess->size > 0 && !fileAccess->ftruncate())
    {
        // Couldn't truncate the file.
        return API_EWRITE;
    }

    // Encrypt the configuration data.
    const string d = encrypt(data);

    // Write the encrypted configuration data.
    auto* bytes = reinterpret_cast<const byte*>(&d[0]);

    if (!fileAccess->fwrite(bytes, d.size(), 0x0))
    {
        // Couldn't write out the data.
        return API_EWRITE;
    }

    return API_OK;
}

bool XBackupConfigIOContext::decrypt(const string& in, string& out)
{
    // Handy constants.
    const size_t IV_LENGTH       = SymmCipher::KEYLENGTH;
    const size_t MAC_LENGTH      = 32;
    const size_t METADATA_LENGTH = IV_LENGTH + MAC_LENGTH;

    // Is the file too short to be valid?
    if (in.size() <= METADATA_LENGTH)
    {
        return false;
    }

    // For convenience.
    const byte* data = reinterpret_cast<const byte*>(&in[0]);
    const byte* iv   = &data[in.size() - METADATA_LENGTH];
    const byte* mac  = &data[in.size() - MAC_LENGTH];

    byte cmac[MAC_LENGTH];

    // Compute HMAC on file.
    mSigner.add(data, in.size() - MAC_LENGTH);
    mSigner.get(cmac);

    // Is the file corrupt?
    if (memcmp(cmac, mac, MAC_LENGTH))
    {
        return false;
    }

    // Try and decrypt the file.
    return mCipher.cbc_decrypt_pkcs_padding(data,
                                            in.size() - METADATA_LENGTH,
                                            iv,
                                            &out);
}

bool XBackupConfigIOContext::deserialize(XBackupConfig& config, JSON& reader) const
{
    const auto TYPE_ENABLED       = MAKENAMEID2('e', 'n');
    const auto TYPE_HEARTBEAT_ID  = MAKENAMEID2('h', 'b');
    const auto TYPE_LAST_ERROR    = MAKENAMEID2('l', 'e');
    const auto TYPE_SOURCE_PATH   = MAKENAMEID2('s', 'p');
    const auto TYPE_TAG           = MAKENAMEID1('t');
    const auto TYPE_TARGET_HANDLE = MAKENAMEID2('t', 'h');

    for ( ; ; )
    {
        switch (reader.getnameid())
        {
        case EOO:
            return true;

        case TYPE_ENABLED:
            config.enabled = reader.getbool();
            break;
 
        case TYPE_HEARTBEAT_ID:
            config.heartbeatID =
              reader.gethandle(sizeof(handle));
            break;

        case TYPE_LAST_ERROR:
            config.lastError =
              static_cast<SyncError>(reader.getint32());
            break;

        case TYPE_SOURCE_PATH:
        {
            string sourcePath;

            reader.storebinary(&sourcePath);
            
            config.sourcePath =
              LocalPath::fromPath(sourcePath, mFsAccess);

            break;
        }

        case TYPE_TAG:
            config.tag = reader.getint32();
            break;

        case TYPE_TARGET_HANDLE:
            config.targetHandle =
              reader.gethandle(sizeof(handle));
            break;

        default:
            if (!reader.storeobject())
            {
                return false;
            }
            break;
        }
    }
}

string XBackupConfigIOContext::encrypt(const string& data)
{
    byte iv[SymmCipher::KEYLENGTH];

    // Generate initialization vector.
    mRNG.genblock(iv, sizeof(iv));

    string d;

    // Encrypt file using IV.
    mCipher.cbc_encrypt_pkcs_padding(&data, iv, &d);

    // Add IV to file.
    d.insert(d.end(), std::begin(iv), std::end(iv));

    byte mac[32];

    // Compute HMAC on file (including IV).
    mSigner.add(reinterpret_cast<const byte*>(&d[0]), d.size());
    mSigner.get(mac);

    // Add HMAC to file.
    d.insert(d.end(), std::begin(mac), std::end(mac));

    // We're done.
    return d;
}

void XBackupConfigIOContext::serialize(const XBackupConfig& config,
                                       JSONWriter& writer) const
{
    // Encode path to avoid escaping issues.
    const string sourcePath =
      Base64::btoa(config.sourcePath.toPath(mFsAccess));

    writer.beginobject();

    writer.arg("sp", sourcePath);
    writer.arg("hb", config.heartbeatID, sizeof(handle));
    writer.arg("th", config.targetHandle, sizeof(handle));
    writer.arg("le", config.lastError);
    writer.arg("t", config.tag);
    writer.arg("en", config.enabled);

    writer.endobject();
}

const string XBackupConfigIOContext::BACKUP_CONFIG_DIR = ".megabackup";

XBackupConfigDBObserver::XBackupConfigDBObserver()
{
}

XBackupConfigDBObserver::~XBackupConfigDBObserver()
{
}

XBackupConfigStore::XBackupConfigStore(SymmCipher& cipher,
                                       FileSystemAccess& fsAccess,
                                       const string& key,
                                       const string& name,
                                       PrnGen& rng)
  : XBackupConfigDBObserver()
  , XBackupConfigIOContext(cipher, fsAccess, key, name, rng)
  , mDirtyDB()
  , mDriveToDB()
  , mTagToDB()
  , mTargetToDB()
{
}

XBackupConfigStore::~XBackupConfigStore()
{
    // Close all open databases.
    close();
}

const XBackupConfig* XBackupConfigStore::add(const XBackupConfig& config)
{
    auto i = mTagToDB.find(config.tag);

    // Is the config already in a database?
    if (i != mTagToDB.end())
    {
        // Is the config moving between databases?
        if (i->second->drivePath() == config.drivePath)
        {
            // Nope, just update the config.
            return i->second->add(config);
        }

        // Remove the config from the (old) database.
        i->second->remove(config.tag);
    }

    auto j = mDriveToDB.find(config.drivePath);

    // Does the target database exist?
    if (j == mDriveToDB.end())
    {
        // Nope, can't add the config.
        return nullptr;
    }

    // Add (update) the config.
    return j->second->add(config);
}

error XBackupConfigStore::close(const LocalPath& drivePath)
{
    auto i = mDriveToDB.find(drivePath);

    // Does the database exist?
    if (i != mDriveToDB.end())
    {
        // Yep, close it.
        return close(*i->second);
    }

    // Doesn't exist.
    return API_ENOENT;
}

error XBackupConfigStore::close()
{
    error result = API_OK;

    auto i = mDriveToDB.begin();
    auto j = mDriveToDB.end();

    // Close all the databases.
    while (i != j)
    {
        auto& db = *i++->second;

        if (close(db) != API_OK)
        {
            result = API_EWRITE;
        }
    }

    return result;
}

const XBackupConfigMap* XBackupConfigStore::configs(const LocalPath& drivePath) const
{
    auto i = mDriveToDB.find(drivePath);

    // Database exist?
    if (i != mDriveToDB.end())
    {
        // Yep, return a reference to its configs.
        return &i->second->configs();
    }

    // Nothing to return.
    return nullptr;
}

XBackupConfigMap XBackupConfigStore::configs() const
{
    XBackupConfigMap result;

    // Collect configs from all databases.
    for (auto& i : mDriveToDB)
    {
        auto& configs = i.second->configs();

        result.insert(configs.begin(), configs.end());
    }

    return result;
}

const XBackupConfigMap* XBackupConfigStore::create(const LocalPath& drivePath)
{
    // Has this database already been opened?
    if (opened(drivePath))
    {
        // Yes, so we have nothing to do.
        return nullptr;
    }

    // Create database object.
    XBackupConfigDBPtr db(new XBackupConfigDB(drivePath, *this));

    // Load existing database, if any.
    if (db->read(*this) == API_EREAD)
    {
        // Couldn't load the database.
        return nullptr;
    }

    // Add database to the store.
    auto it = mDriveToDB.emplace(drivePath, std::move(db));

    // Return reference to (possibly empty) configs.
    return &it.first->second->configs();
}

bool XBackupConfigStore::dirty() const
{
    return !mDirtyDB.empty();
}

error XBackupConfigStore::flush(const LocalPath& drivePath)
{
    auto i = mDriveToDB.find(drivePath);

    // Does the database exist?
    if (i != mDriveToDB.end())
    {
        // Yep, flush it.
        return flush(*i->second);
    }

    // Can't flush a database that doesn't exist.
    return API_ENOENT;
}

error XBackupConfigStore::flush(vector<LocalPath>& drivePaths)
{
    error result = API_OK;

    // Try and flush all dirty databases.
    auto i = mDirtyDB.begin();
    auto j = mDirtyDB.end();

    while (i != j)
    {
        auto* db = *i++;

        if (flush(*db) != API_OK)
        {
            // Record which databases couldn't be flushed.
            drivePaths.emplace_back(db->drivePath());

            result = API_EWRITE;
        }
    }

    return result;
}

error XBackupConfigStore::flush()
{
    error result = API_OK;

    // Try and flush all dirty databases.
    for (auto* db : mDirtyDB)
    {
        if (flush(*db) != API_OK)
        {
            result = API_EWRITE;
        }
    }

    return result;
}

const XBackupConfig* XBackupConfigStore::get(const int tag) const
{
    auto it = mTagToDB.find(tag);

    if (it != mTagToDB.end())
    {
        return it->second->get(tag);
    }
    
    return nullptr;
}

const XBackupConfig* XBackupConfigStore::get(const handle targetHandle) const
{
    auto it = mTargetToDB.find(targetHandle);

    if (it != mTargetToDB.end())
    {
        return it->second->get(targetHandle);
    }

    return nullptr;
}

const XBackupConfigMap* XBackupConfigStore::open(const LocalPath& drivePath)
{
    // Has this database already been opened?
    if (opened(drivePath))
    {
        // Yep, do nothing.
        return nullptr;
    }

    // Create database object.
    XBackupConfigDBPtr db(new XBackupConfigDB(drivePath, *this));

    // Try and load the database from disk.
    if (db->read(*this) != API_OK)
    {
        // Couldn't load the database.
        return nullptr;
    }

    // Add the database to the store.
    auto it = mDriveToDB.emplace(drivePath, std::move(db));

    // Return reference to (possibly empty) configs.
    return &it.first->second->configs();
}

bool XBackupConfigStore::opened(const LocalPath& drivePath) const
{
    return mDriveToDB.count(drivePath) > 0;
}

error XBackupConfigStore::remove(const int tag)
{
    auto it = mTagToDB.find(tag);

    if (it != mTagToDB.end())
    {
        return it->second->remove(tag);
    }

    return API_ENOENT;
}

error XBackupConfigStore::remove(const handle targetHandle)
{
    auto it = mTargetToDB.find(targetHandle);

    if (it != mTargetToDB.end())
    {
        return it->second->remove(targetHandle);
    }

    return API_ENOENT;
}

error XBackupConfigStore::close(XBackupConfigDB& db)
{
    // Try and flush the database.
    const auto result = flush(db);

    // Remove the database from memory.
    mDriveToDB.erase(db.drivePath());

    // Return flush result.
    return result;
}

error XBackupConfigStore::flush(XBackupConfigDB& db)
{
    auto i = mDirtyDB.find(&db);

    // Does this database need flushing?
    if (i == mDirtyDB.end())
    {
        // Doesn't need flushing.
        return API_OK;
    }

    // Try and write the database to disk.
    auto result = (*i)->write(*this);

    // Database no longer needs flushing.
    mDirtyDB.erase(i);

    return result;
}

void XBackupConfigStore::onAdd(XBackupConfigDB& db, const XBackupConfig& config)
{
    mTagToDB.emplace(config.tag, &db);

    if (config.targetHandle == UNDEF)
    {
        return;
    }

    // Sanity check.
    assert(mTargetToDB.count(config.targetHandle) == 0);

    mTargetToDB.emplace(config.targetHandle, &db);
}

void XBackupConfigStore::onChange(XBackupConfigDB& db,
                                  const XBackupConfig& from,
                                  const XBackupConfig& to)
{
    mTargetToDB.erase(from.targetHandle);

    if (to.targetHandle == UNDEF)
    {
        return;
    }

    // Sanity check.
    assert(mTargetToDB.count(to.targetHandle) == 0);

    mTargetToDB.emplace(to.targetHandle, &db);
}

void XBackupConfigStore::onDirty(XBackupConfigDB& db)
{
    mDirtyDB.emplace(&db);
}

void XBackupConfigStore::onRemove(XBackupConfigDB&, const XBackupConfig& config)
{
    mTagToDB.erase(config.tag);
    mTargetToDB.erase(config.targetHandle);
}

} // namespace

#endif

