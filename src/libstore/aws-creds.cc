#include "nix/store/aws-creds.hh"

#if NIX_WITH_AWS_AUTH

#  include "aws-creds-module.hh"
#  include "store-config-private.hh"
#  include "nix/store/s3-url.hh"
#  include "nix/util/logging.hh"

#  include <aws/common/logging.h>

#  if NIX_AWS_AS_MODULE
#    include <dlfcn.h>
#  endif

namespace nix {

namespace {

using aws_module::Result;

/**
 * Map AWS log level to Nix verbosity.
 * AWS levels: AWS_LL_NONE=0, AWS_LL_FATAL=1, AWS_LL_ERROR=2, AWS_LL_WARN=3,
 *             AWS_LL_INFO=4, AWS_LL_DEBUG=5, AWS_LL_TRACE=6
 *
 * We map very conservatively because the AWS SDK is extremely noisy. What AWS
 * considers "info" includes low-level details like "Initializing epoll" and
 * "Starting event-loop thread". What it considers "errors" includes expected
 * conditions like missing ~/.aws/config or IMDS being unavailable on non-EC2.
 *
 * To avoid spamming users, we only show FATAL at default verbosity. Everything
 * else requires -vvvvv (lvlDebug) or higher to see.
 */
Verbosity awsLogLevelToVerbosity(enum aws_log_level level)
{
    switch (level) {
    case AWS_LL_FATAL:
        return lvlError;
    case AWS_LL_ERROR:
    case AWS_LL_WARN:
    case AWS_LL_INFO:
        return lvlDebug;
    case AWS_LL_DEBUG:
    case AWS_LL_TRACE:
        return lvlVomit;
    // AWS_LL_NONE and AWS_LL_COUNT are enum sentinels, not real log levels
    case AWS_LL_NONE:
    case AWS_LL_COUNT:
        return lvlDebug;
    }
    unreachable();
}

const aws_module::Host host{
    /* Map Nix verbosity back to AWS log level (inverse of awsLogLevelToVerbosity) */
    .getLogLevel = []() -> int {
        if (verbosity >= lvlVomit)
            return AWS_LL_TRACE;
        if (verbosity >= lvlDebug)
            return AWS_LL_INFO; // error/warn/info are all mapped to lvlDebug
        return AWS_LL_FATAL;
    },
    .log =
        [](int awsLevel, const char * subject, const char * message) {
            Verbosity nixLevel = awsLogLevelToVerbosity(aws_log_level(awsLevel));
            printMsgUsing(nix::logger, nixLevel, "(aws:%s) %s", subject, chomp(message));
        },
    .debug = [](const std::string & message) { debug("%s", message); },
    .warn = [](const std::string & message) { warn("%s", message); },
};

struct Module
{
    aws_module::MakeProvider makeProvider;
    aws_module::ErrorString errorString;
};

const Module & loadModule()
{
#  if !NIX_AWS_AS_MODULE
    static Module module{nixAwsMakeProvider, nixAwsErrorString};
#  else
    static Module module = [] {
        auto handle = dlopen(NIX_AWS_MODULE_PATH, RTLD_NOW | RTLD_LOCAL);
        if (!handle)
            throw Error("could not load the AWS authentication module: %s", dlerror());
        Module module{
            reinterpret_cast<aws_module::MakeProvider>(dlsym(handle, "nixAwsMakeProvider")),
            reinterpret_cast<aws_module::ErrorString>(dlsym(handle, "nixAwsErrorString")),
        };
        if (!module.makeProvider || !module.errorString)
            throw Error("could not load the AWS authentication module %s: %s", NIX_AWS_MODULE_PATH, dlerror());
        return module;
    }();
#  endif
    return module;
}

} // anonymous namespace

void AwsAuthError::anchor() {}

AwsAuthError::AwsAuthError(int errorCode)
    : CloneableError("AWS authentication error: '%s' (%d)", loadModule().errorString(errorCode), errorCode)
    , errorCode(errorCode)
{
}

AwsCredentialProvider::~AwsCredentialProvider() {}

namespace {

class AwsCredentialProviderImpl : public AwsCredentialProvider
{
    std::unique_ptr<aws_module::Provider> module;

public:
    AwsCredentialProviderImpl()
    {
        Result error;
        module.reset(loadModule().makeProvider(aws_module::abiVersion, &host, &error));
        if (!module) {
            if (error.kind == Result::BootstrapFailed)
                throw AwsAuthError("failed to create AWS client bootstrap");
            throw Error("the AWS authentication module does not match this Nix");
        }
    }

    AwsCredentials getCredentialsRaw(const std::string & profile, const std::string & region)
    {
        auto res = module->getCredentials(profile, region);
        switch (res.kind) {
        case Result::Ok:
            return AwsCredentials(res.accessKeyId, res.secretAccessKey, res.sessionToken);
        case Result::AwsError:
            throw AwsAuthError(res.errorCode);
        case Result::ProviderInvalid:
            throw AwsAuthError("AWS credential provider is invalid");
        case Result::Timeout:
            throw AwsAuthError("Timeout waiting for AWS credentials (%d seconds)", res.errorCode);
        case Result::ProviderCreationFailed:
        case Result::BootstrapFailed:
            break;
        }
        throw AwsAuthError(
            "Failed to create AWS credentials provider for %s",
            profile.empty() ? "default profile" : fmt("profile '%s'", profile));
    }

    AwsCredentials getCredentials(const ParsedS3URL & url) override
    {
        auto profile = url.profile.value_or("");
        auto region = url.region.value_or("");
        try {
            return getCredentialsRaw(profile, region);
        } catch (AwsAuthError & e) {
            warn("AWS authentication failed for S3 request %s: %s", url.toHttpsUrl(), e.message());
            module->forget(profile, region);
            throw;
        }
    }
};

} // anonymous namespace

ref<AwsCredentialProvider> makeAwsCredentialsProvider()
{
    return make_ref<AwsCredentialProviderImpl>();
}

ref<AwsCredentialProvider> getAwsCredentialsProvider()
{
    static auto instance = makeAwsCredentialsProvider();
    return instance;
}

} // namespace nix

#endif
