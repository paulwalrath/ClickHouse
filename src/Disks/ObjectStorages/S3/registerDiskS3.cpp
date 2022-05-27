#include <Common/config.h>

#include <Common/logger_useful.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>
#include <Interpreters/Context.h>
#include <Disks/DiskFactory.h>

#if USE_AWS_S3

#include <aws/core/client/DefaultRetryStrategy.h>

#include <base/getFQDNOrHostName.h>

#include <Disks/DiskRestartProxy.h>
#include <Disks/DiskLocal.h>

#include <Disks/ObjectStorages/DiskObjectStorage.h>
#include <Disks/ObjectStorages/DiskObjectStorageCommon.h>
#include <Disks/ObjectStorages/S3/ProxyConfiguration.h>
#include <Disks/ObjectStorages/S3/ProxyListConfiguration.h>
#include <Disks/ObjectStorages/S3/ProxyResolverConfiguration.h>
#include <Disks/ObjectStorages/S3/S3ObjectStorage.h>
#include <Disks/ObjectStorages/S3/diskSettings.h>

#include <IO/S3Common.h>

#include <Storages/StorageS3Settings.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int PATH_ACCESS_DENIED;
}

namespace
{

void checkWriteAccess(IDisk & disk)
{
    auto file = disk.writeFile("test_acl", DBMS_DEFAULT_BUFFER_SIZE, WriteMode::Rewrite);
    try
    {
        file->write("test", 4);
    }
    catch (...)
    {
        file->finalize();
        throw;
    }
}

void checkReadAccess(const String & disk_name, IDisk & disk)
{
    auto file = disk.readFile("test_acl");
    String buf(4, '0');
    file->readStrict(buf.data(), 4);
    if (buf != "test")
        throw Exception("No read access to S3 bucket in disk " + disk_name, ErrorCodes::PATH_ACCESS_DENIED);
}

void checkRemoveAccess(IDisk & disk) { disk.removeFile("test_acl"); }

}

void registerDiskS3(DiskFactory & factory)
{
    auto creator = [](const String & name,
                      const Poco::Util::AbstractConfiguration & config,
                      const String & config_prefix,
                      ContextPtr context,
                      const DisksMap & /*map*/) -> DiskPtr {
        S3::URI uri(Poco::URI(config.getString(config_prefix + ".endpoint")));

        if (uri.key.empty())
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "No key in S3 uri: {}", uri.uri.toString());

        if (uri.key.back() != '/')
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "S3 path must ends with '/', but '{}' doesn't.", uri.key);

        auto [metadata_path, metadata_disk] = prepareForLocalMetadata(name, config, config_prefix, context);

        ObjectStoragePtr s3_storage = std::make_unique<S3ObjectStorage>(
            getClient(config, config_prefix, context),
            getSettings(config, config_prefix, context),
            uri.version_id, uri.bucket);

        bool send_metadata = config.getBool(config_prefix + ".send_metadata", false);
        uint64_t copy_thread_pool_size = config.getUInt(config_prefix + ".thread_pool_size", 16);

        std::shared_ptr<DiskObjectStorage> s3disk = std::make_shared<DiskObjectStorage>(
            name,
            uri.key,
            "DiskS3",
            metadata_disk,
            std::move(s3_storage),
            DiskType::S3,
            send_metadata,
            copy_thread_pool_size);

        /// This code is used only to check access to the corresponding disk.
        if (!config.getBool(config_prefix + ".skip_access_check", false))
        {
            checkWriteAccess(*s3disk);
            checkReadAccess(name, *s3disk);
            checkRemoveAccess(*s3disk);
        }

        s3disk->startup(context);

        std::shared_ptr<IDisk> disk_result = s3disk;

        return std::make_shared<DiskRestartProxy>(disk_result);
    };
    factory.registerDiskType("s3", creator);
}

}

#else

void registerDiskS3(DiskFactory &) {}

#endif
