/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/replset/sharding_catalog_manager_impl.h"

#include <iomanip>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/set_shard_version_request.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

using std::string;
using std::vector;
using str::stream;

namespace {

const ReadPreferenceSetting kConfigReadSelector(ReadPreference::Nearest, TagSet{});
const WriteConcernOptions kNoWaitWriteConcern(1, WriteConcernOptions::SyncMode::UNSET, Seconds(0));

void toBatchError(const Status& status, BatchedCommandResponse* response) {
    response->clear();
    response->setErrCode(status.code());
    response->setErrMessage(status.reason());
    response->setOk(false);
}

const ResourceId kZoneOpResourceId(RESOURCE_METADATA, "$configZonedSharding"_sd);

/**
 * Lock for shard zoning operations. This should be acquired when doing any operations that
 * can afffect the config.tags (or the tags field of the config.shards) collection.
 */
class ScopedZoneOpExclusiveLock {
public:
    ScopedZoneOpExclusiveLock(OperationContext* txn)
        : _transaction(txn, MODE_IX),
          _globalIXLock(txn->lockState(), MODE_IX, UINT_MAX),
          // Grab global lock recursively so locks will not be yielded.
          _recursiveGlobalIXLock(txn->lockState(), MODE_IX, UINT_MAX),
          _zoneLock(txn->lockState(), kZoneOpResourceId, MODE_X) {}

private:
    ScopedTransaction _transaction;
    Lock::GlobalLock _globalIXLock;
    Lock::GlobalLock _recursiveGlobalIXLock;
    Lock::ResourceLock _zoneLock;
};

}  // namespace


ShardingCatalogManagerImpl::ShardingCatalogManagerImpl(
    ShardingCatalogClient* catalogClient, std::unique_ptr<executor::TaskExecutor> addShardExecutor)
    : _catalogClient(catalogClient), _executorForAddShard(std::move(addShardExecutor)) {}

ShardingCatalogManagerImpl::~ShardingCatalogManagerImpl() = default;

Status ShardingCatalogManagerImpl::startup() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_started) {
        return Status::OK();
    }
    _started = true;
    _executorForAddShard->startup();
    return Status::OK();
}

void ShardingCatalogManagerImpl::shutDown(OperationContext* txn) {
    LOG(1) << "ShardingCatalogManagerImpl::shutDown() called.";
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _inShutdown = true;
    }

    _executorForAddShard->shutdown();
    _executorForAddShard->join();
}

StatusWith<Shard::CommandResponse> ShardingCatalogManagerImpl::_runCommandForAddShard(
    OperationContext* txn,
    RemoteCommandTargeter* targeter,
    const std::string& dbName,
    const BSONObj& cmdObj) {
    auto host = targeter->findHost(ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                   RemoteCommandTargeter::selectFindHostMaxWaitTime(txn));
    if (!host.isOK()) {
        return host.getStatus();
    }

    executor::RemoteCommandRequest request(
        host.getValue(), dbName, cmdObj, rpc::makeEmptyMetadata(), Seconds(30));
    StatusWith<executor::RemoteCommandResponse> swResponse =
        Status(ErrorCodes::InternalError, "Internal error running command");

    auto callStatus = _executorForAddShard->scheduleRemoteCommand(
        request, [&swResponse](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
            swResponse = args.response;
        });
    if (!callStatus.isOK()) {
        return callStatus.getStatus();
    }

    // Block until the command is carried out
    _executorForAddShard->wait(callStatus.getValue());

    if (!swResponse.isOK()) {
        if (swResponse.getStatus().compareCode(ErrorCodes::ExceededTimeLimit)) {
            LOG(0) << "Operation for addShard timed out with status " << swResponse.getStatus();
        }
        return swResponse.getStatus();
    }

    BSONObj responseObj = swResponse.getValue().data.getOwned();
    BSONObj responseMetadata = swResponse.getValue().metadata.getOwned();
    Status commandStatus = getStatusFromCommandResult(responseObj);
    Status writeConcernStatus = getWriteConcernStatusFromCommandResult(responseObj);

    return Shard::CommandResponse(std::move(responseObj),
                                  std::move(responseMetadata),
                                  std::move(commandStatus),
                                  std::move(writeConcernStatus));
}

StatusWith<ShardType> ShardingCatalogManagerImpl::_validateHostAsShard(
    OperationContext* txn,
    std::shared_ptr<RemoteCommandTargeter> targeter,
    const std::string* shardProposedName,
    const ConnectionString& connectionString) {
    // Check whether any host in the connection is already part of the cluster.
    Grid::get(txn)->shardRegistry()->reload(txn);
    for (const auto& hostAndPort : connectionString.getServers()) {
        std::shared_ptr<Shard> shard;
        shard = Grid::get(txn)->shardRegistry()->getShardNoReload(hostAndPort.toString());
        if (shard) {
            return {ErrorCodes::OperationFailed,
                    str::stream() << "'" << hostAndPort.toString() << "' "
                                  << "is already a member of the existing shard '"
                                  << shard->getConnString().toString()
                                  << "' ("
                                  << shard->getId()
                                  << ")."};
        }
    }

    // Check for mongos and older version mongod connections, and whether the hosts
    // can be found for the user specified replset.
    auto swCommandResponse =
        _runCommandForAddShard(txn, targeter.get(), "admin", BSON("isMaster" << 1));
    if (!swCommandResponse.isOK()) {
        if (swCommandResponse.getStatus() == ErrorCodes::RPCProtocolNegotiationFailed) {
            // Mongos to mongos commands are no longer supported in the wire protocol
            // (because mongos does not support OP_COMMAND), similarly for a new mongos
            // and an old mongod. So the call will fail in such cases.
            // TODO: If/When mongos ever supports opCommands, this logic will break because
            // cmdStatus will be OK.
            return {ErrorCodes::RPCProtocolNegotiationFailed,
                    str::stream() << targeter->connectionString().toString()
                                  << " does not recognize the RPC protocol being used. This is"
                                  << " likely because it contains a node that is a mongos or an old"
                                  << " version of mongod."};
        } else {
            return swCommandResponse.getStatus();
        }
    }

    // Check for a command response error
    auto resIsMasterStatus = std::move(swCommandResponse.getValue().commandStatus);
    if (!resIsMasterStatus.isOK()) {
        return {resIsMasterStatus.code(),
                str::stream() << "Error running isMaster against "
                              << targeter->connectionString().toString()
                              << ": "
                              << causedBy(resIsMasterStatus)};
    }

    auto resIsMaster = std::move(swCommandResponse.getValue().response);

    // Check whether there is a master. If there isn't, the replica set may not have been
    // initiated. If the connection is a standalone, it will return true for isMaster.
    bool isMaster;
    Status status = bsonExtractBooleanField(resIsMaster, "ismaster", &isMaster);
    if (!status.isOK()) {
        return Status(status.code(),
                      str::stream() << "isMaster returned invalid 'ismaster' "
                                    << "field when attempting to add "
                                    << connectionString.toString()
                                    << " as a shard: "
                                    << status.reason());
    }
    if (!isMaster) {
        return {ErrorCodes::NotMaster,
                str::stream()
                    << connectionString.toString()
                    << " does not have a master. If this is a replica set, ensure that it has a"
                    << " healthy primary and that the set has been properly initiated."};
    }

    const string providedSetName = connectionString.getSetName();
    const string foundSetName = resIsMaster["setName"].str();

    // Make sure the specified replica set name (if any) matches the actual shard's replica set
    if (providedSetName.empty() && !foundSetName.empty()) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "host is part of set " << foundSetName << "; "
                              << "use replica set url format "
                              << "<setname>/<server1>,<server2>, ..."};
    }

    if (!providedSetName.empty() && foundSetName.empty()) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "host did not return a set name; "
                              << "is the replica set still initializing? "
                              << resIsMaster};
    }

    // Make sure the set name specified in the connection string matches the one where its hosts
    // belong into
    if (!providedSetName.empty() && (providedSetName != foundSetName)) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "the provided connection string (" << connectionString.toString()
                              << ") does not match the actual set name "
                              << foundSetName};
    }

    // Is it a config server?
    if (resIsMaster.hasField("configsvr")) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "Cannot add " << connectionString.toString()
                              << " as a shard since it is a config server"};
    }

    // If the shard is part of a replica set, make sure all the hosts mentioned in the connection
    // string are part of the set. It is fine if not all members of the set are mentioned in the
    // connection string, though.
    if (!providedSetName.empty()) {
        std::set<string> hostSet;

        BSONObjIterator iter(resIsMaster["hosts"].Obj());
        while (iter.more()) {
            hostSet.insert(iter.next().String());  // host:port
        }

        if (resIsMaster["passives"].isABSONObj()) {
            BSONObjIterator piter(resIsMaster["passives"].Obj());
            while (piter.more()) {
                hostSet.insert(piter.next().String());  // host:port
            }
        }

        if (resIsMaster["arbiters"].isABSONObj()) {
            BSONObjIterator piter(resIsMaster["arbiters"].Obj());
            while (piter.more()) {
                hostSet.insert(piter.next().String());  // host:port
            }
        }

        vector<HostAndPort> hosts = connectionString.getServers();
        for (size_t i = 0; i < hosts.size(); i++) {
            const string host = hosts[i].toString();  // host:port
            if (hostSet.find(host) == hostSet.end()) {
                return {ErrorCodes::OperationFailed,
                        str::stream() << "in seed list " << connectionString.toString() << ", host "
                                      << host
                                      << " does not belong to replica set "
                                      << foundSetName
                                      << "; found "
                                      << resIsMaster.toString()};
            }
        }
    }

    string actualShardName;

    if (shardProposedName) {
        actualShardName = *shardProposedName;
    } else if (!foundSetName.empty()) {
        // Default it to the name of the replica set
        actualShardName = foundSetName;
    }

    // Disallow adding shard replica set with name 'config'
    if (actualShardName == "config") {
        return {ErrorCodes::BadValue, "use of shard replica set with name 'config' is not allowed"};
    }

    // Retrieve the most up to date connection string that we know from the replica set monitor (if
    // this is a replica set shard, otherwise it will be the same value as connectionString).
    ConnectionString actualShardConnStr = targeter->connectionString();

    ShardType shard;
    shard.setName(actualShardName);
    shard.setHost(actualShardConnStr.toString());

    return shard;
}

StatusWith<std::vector<std::string>> ShardingCatalogManagerImpl::_getDBNamesListFromShard(
    OperationContext* txn, std::shared_ptr<RemoteCommandTargeter> targeter) {

    auto swCommandResponse =
        _runCommandForAddShard(txn, targeter.get(), "admin", BSON("listDatabases" << 1));
    if (!swCommandResponse.isOK()) {
        return swCommandResponse.getStatus();
    }

    auto cmdStatus = std::move(swCommandResponse.getValue().commandStatus);
    if (!cmdStatus.isOK()) {
        return cmdStatus;
    }

    auto cmdResult = std::move(swCommandResponse.getValue().response);

    vector<string> dbNames;

    for (const auto& dbEntry : cmdResult["databases"].Obj()) {
        const string& dbName = dbEntry["name"].String();

        if (!(dbName == "local" || dbName == "admin")) {
            dbNames.push_back(dbName);
        }
    }

    return dbNames;
}

StatusWith<std::string> ShardingCatalogManagerImpl::_generateNewShardName(OperationContext* txn) {
    BSONObjBuilder shardNameRegex;
    shardNameRegex.appendRegex(ShardType::name(), "^shard");

    auto findStatus = Grid::get(txn)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
        txn,
        kConfigReadSelector,
        repl::ReadConcernLevel::kMajorityReadConcern,
        NamespaceString(ShardType::ConfigNS),
        shardNameRegex.obj(),
        BSON(ShardType::name() << -1),
        1);
    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& docs = findStatus.getValue().docs;

    int count = 0;
    if (!docs.empty()) {
        const auto shardStatus = ShardType::fromBSON(docs.front());
        if (!shardStatus.isOK()) {
            return shardStatus.getStatus();
        }

        std::istringstream is(shardStatus.getValue().getName().substr(5));
        is >> count;
        count++;
    }

    // TODO fix so that we can have more than 10000 automatically generated shard names
    if (count < 9999) {
        std::stringstream ss;
        ss << "shard" << std::setfill('0') << std::setw(4) << count;
        return ss.str();
    }

    return Status(ErrorCodes::OperationFailed, "unable to generate new shard name");
}

StatusWith<string> ShardingCatalogManagerImpl::addShard(
    OperationContext* txn,
    const std::string* shardProposedName,
    const ConnectionString& shardConnectionString,
    const long long maxSize) {
    if (shardConnectionString.type() == ConnectionString::INVALID) {
        return {ErrorCodes::BadValue, "Invalid connection string"};
    }

    if (shardProposedName && shardProposedName->empty()) {
        return {ErrorCodes::BadValue, "shard name cannot be empty"};
    }

    // TODO: Don't create a detached Shard object, create a detached RemoteCommandTargeter instead.
    const std::shared_ptr<Shard> shard{
        Grid::get(txn)->shardRegistry()->createConnection(shardConnectionString)};
    invariant(shard);
    auto targeter = shard->getTargeter();

    // Validate the specified connection string may serve as shard at all
    auto shardStatus =
        _validateHostAsShard(txn, targeter, shardProposedName, shardConnectionString);
    if (!shardStatus.isOK()) {
        // TODO: This is a workaround for the case were we could have some bad shard being
        // requested to be added and we put that bad connection string on the global replica set
        // monitor registry. It needs to be cleaned up so that when a correct replica set is added,
        // it will be recreated.
        ReplicaSetMonitor::remove(shardConnectionString.getSetName());
        return shardStatus.getStatus();
    }

    ShardType& shardType = shardStatus.getValue();

    auto dbNamesStatus = _getDBNamesListFromShard(txn, targeter);
    if (!dbNamesStatus.isOK()) {
        return dbNamesStatus.getStatus();
    }

    // Check that none of the existing shard candidate's dbs exist already
    for (const string& dbName : dbNamesStatus.getValue()) {
        auto dbt = _catalogClient->getDatabase(txn, dbName);
        if (dbt.isOK()) {
            const auto& dbDoc = dbt.getValue().value;
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "can't add shard "
                                        << "'"
                                        << shardConnectionString.toString()
                                        << "'"
                                        << " because a local database '"
                                        << dbName
                                        << "' exists in another "
                                        << dbDoc.getPrimary());
        } else if (dbt != ErrorCodes::NamespaceNotFound) {
            return dbt.getStatus();
        }
    }

    // If a name for a shard wasn't provided, generate one
    if (shardType.getName().empty()) {
        StatusWith<string> result = _generateNewShardName(txn);
        if (!result.isOK()) {
            return result.getStatus();
        }
        shardType.setName(result.getValue());
    }

    if (maxSize > 0) {
        shardType.setMaxSizeMB(maxSize);
    }

    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        Grid::get(txn)->shardRegistry()->getConfigServerConnectionString());
    shardIdentity.setShardName(shardType.getName());
    shardIdentity.setClusterId(Grid::get(txn)->shardRegistry()->getClusterId());
    auto validateStatus = shardIdentity.validate();
    if (!validateStatus.isOK()) {
        return validateStatus;
    }

    log() << "going to insert shardIdentity document into shard: " << shardIdentity.toString();

    auto updateRequest = shardIdentity.createUpsertForAddShard();
    BatchedCommandRequest commandRequest(updateRequest.release());
    commandRequest.setNS(NamespaceString::kConfigCollectionNamespace);
    commandRequest.setWriteConcern(ShardingCatalogClient::kMajorityWriteConcern.toBSON());

    auto swCommandResponse =
        _runCommandForAddShard(txn, targeter.get(), "admin", commandRequest.toBSON());

    if (!swCommandResponse.isOK()) {
        return swCommandResponse.getStatus();
    }

    auto commandResponse = std::move(swCommandResponse.getValue());

    BatchedCommandResponse batchResponse;
    auto batchResponseStatus =
        Shard::CommandResponse::processBatchWriteResponse(commandResponse, &batchResponse);
    if (!batchResponseStatus.isOK()) {
        return batchResponseStatus;
    }

    log() << "going to insert new entry for shard into config.shards: " << shardType.toString();

    Status result = _catalogClient->insertConfigDocument(
        txn, ShardType::ConfigNS, shardType.toBSON(), ShardingCatalogClient::kMajorityWriteConcern);
    if (!result.isOK()) {
        log() << "error adding shard: " << shardType.toBSON() << " err: " << result.reason();
        if (result == ErrorCodes::DuplicateKey) {
            // TODO(SERVER-24213): adding a shard that already exists should be considered success,
            // however this approach does no validation that we are adding the shard with the same
            // options.  It also does not protect against adding the same shard with a different
            // shard name and slightly different connection string.  This is a temporary hack to
            // get the continuous stepdown suite passing.
            warning() << "Received duplicate key error when inserting new shard with name "
                      << shardType.getName() << " and connection string "
                      << shardConnectionString.toString()
                      << " to config.shards collection.  This most likely means that there was an "
                         "attempt to add a shard that already exists in the cluster";
            return shardType.getName();
        }
        return result;
    }

    // Add all databases which were discovered on the new shard
    for (const string& dbName : dbNamesStatus.getValue()) {
        DatabaseType dbt;
        dbt.setName(dbName);
        dbt.setPrimary(shardType.getName());
        dbt.setSharded(false);

        Status status = _catalogClient->updateDatabase(txn, dbName, dbt);
        if (!status.isOK()) {
            log() << "adding shard " << shardConnectionString.toString()
                  << " even though could not add database " << dbName;
        }
    }

    // Record in changelog
    BSONObjBuilder shardDetails;
    shardDetails.append("name", shardType.getName());
    shardDetails.append("host", shardConnectionString.toString());

    _catalogClient->logChange(txn, "addShard", "", shardDetails.obj());

    // Ensure the added shard is visible to this process.
    auto shardRegistry = Grid::get(txn)->shardRegistry();
    if (!shardRegistry->getShard(txn, shardType.getName())) {
        return {ErrorCodes::OperationFailed,
                "Could not find shard metadata for shard after adding it. This most likely "
                "indicates that the shard was removed immediately after it was added."};
    }

    return shardType.getName();
}

Status ShardingCatalogManagerImpl::addShardToZone(OperationContext* txn,
                                                  const std::string& shardName,
                                                  const std::string& zoneName) {
    ScopedZoneOpExclusiveLock scopedLock(txn);

    auto updateStatus = _catalogClient->updateConfigDocument(
        txn,
        ShardType::ConfigNS,
        BSON(ShardType::name(shardName)),
        BSON("$addToSet" << BSON(ShardType::tags() << zoneName)),
        false,
        kNoWaitWriteConcern);

    if (!updateStatus.isOK()) {
        return updateStatus.getStatus();
    }

    if (!updateStatus.getValue()) {
        return {ErrorCodes::ShardNotFound,
                str::stream() << "shard " << shardName << " does not exist"};
    }

    return Status::OK();
}

void ShardingCatalogManagerImpl::appendConnectionStats(executor::ConnectionPoolStats* stats) {
    _executorForAddShard->appendConnectionStats(stats);
}

}  // namespace mongo
