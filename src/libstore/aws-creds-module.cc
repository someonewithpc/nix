#include "aws-creds-module.hh"

#include <aws/crt/Types.h>
#include <aws/crt/Api.h>
#include <aws/crt/auth/Credentials.h>
#include <aws/crt/io/Bootstrap.h>

// C library headers for SSO, STS WebIdentity, and ECS credential providers
#include <aws/auth/credentials.h>

// C library headers for custom logging
#include <aws/common/logging.h>

#include <cstdarg>
#include <cstdlib>

#include <boost/unordered/concurrent_flat_map.hpp>

#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <unistd.h>

namespace nix::aws_module {

namespace {

const Host * host;

/**
 * Custom AWS logger that routes logs through Nix's logging infrastructure.
 *
 * The AWS CRT C++ wrapper (ApiHandle::InitializeLogging) only supports FILE*
 * or filename-based logging. The underlying C library supports custom loggers
 * via aws_logger struct with a vtable containing callback functions.
 */
int nixAwsLoggerLog(
    struct aws_logger * logger, enum aws_log_level logLevel, aws_log_subject_t subject, const char * format, ...)
{
    if (logLevel > host->getLogLevel())
        return AWS_OP_SUCCESS; /* Bail out early to avoid formatting the message unnecessarily. */

    va_list args;
    va_start(args, format);
    std::array<char, 4096> buffer{};
    auto res = vsnprintf(buffer.data(), buffer.size(), format, args);
    va_end(args);
    if (res < 0) /* Skip garbage debug messages in case the SDK is busted. */
        return AWS_OP_SUCCESS;

    const char * subjectName = aws_log_subject_name(subject);
    host->log(logLevel, subjectName ? subjectName : "unknown", buffer.data());
    return AWS_OP_SUCCESS;
}

aws_log_level nixAwsLoggerGetLevel(struct aws_logger * logger, aws_log_subject_t subject)
{
    return aws_log_level(host->getLogLevel());
}

void initialiseAwsLogger()
{
    static std::once_flag initialised; /* aws_logger_set must only be called once */
    std::call_once(initialised, []() {
        static aws_logger_vtable nixAwsLoggerVtable = {
            .log = nixAwsLoggerLog,
            .get_log_level = nixAwsLoggerGetLevel,
            .clean_up = [](struct aws_logger *) {}, // No resources to clean up
            .set_log_level = nullptr,
        };

        static aws_logger nixAwsLogger = {
            .vtable = &nixAwsLoggerVtable,
            .allocator = nullptr,
            .p_impl = nullptr,
        };

        aws_logger_set(&nixAwsLogger);
    });
}

/**
 * Helper function to wrap a C credentials provider in the C++ interface.
 * This replicates the static s_CreateWrappedProvider from aws-crt-cpp.
 */
std::shared_ptr<Aws::Crt::Auth::ICredentialsProvider> createWrappedProvider(
    aws_credentials_provider * rawProvider, Aws::Crt::Allocator * allocator = Aws::Crt::ApiAllocator())
{
    if (rawProvider == nullptr) {
        return nullptr;
    }

    auto provider = Aws::Crt::MakeShared<Aws::Crt::Auth::CredentialsProvider>(allocator, rawProvider, allocator);
    return std::static_pointer_cast<Aws::Crt::Auth::ICredentialsProvider>(provider);
}

/**
 * Create an SSO credentials provider using the C library directly.
 * The C++ wrapper doesn't expose SSO, so we call the C library and wrap the result.
 * Returns nullptr if SSO provider creation fails (e.g., profile doesn't have SSO config).
 */
std::shared_ptr<Aws::Crt::Auth::ICredentialsProvider> createSSOProvider(
    const std::string & profileName,
    Aws::Crt::Io::ClientBootstrap * bootstrap,
    Aws::Crt::Io::TlsContext * tlsContext,
    Aws::Crt::Allocator * allocator = Aws::Crt::ApiAllocator())
{
    aws_credentials_provider_sso_options options;
    AWS_ZERO_STRUCT(options);

    options.bootstrap = bootstrap->GetUnderlyingHandle();
    options.tls_ctx = tlsContext ? tlsContext->GetUnderlyingHandle() : nullptr;
    if (!profileName.empty()) {
        options.profile_name_override = aws_byte_cursor_from_c_str(profileName.c_str());
    }

    // Create the SSO provider - will return nullptr if SSO isn't configured for this profile
    // createWrappedProvider handles nullptr gracefully
    return createWrappedProvider(aws_credentials_provider_new_sso(allocator, &options), allocator);
}

bool envNonEmpty(const char * name)
{
    auto value = std::getenv(name);
    return value && *value;
}

/**
 * Check whether the AWS SDK can resolve a region from the standard
 * environment variables. Mirrors aws_credentials_provider_resolve_region_from_env.
 */
bool awsRegionSetInEnv()
{
    return envNonEmpty("AWS_REGION") || envNonEmpty("AWS_DEFAULT_REGION");
}

/**
 * Create an STS WebIdentity credentials provider using the C library directly.
 * This reads AWS_WEB_IDENTITY_TOKEN_FILE, AWS_ROLE_ARN, AWS_ROLE_SESSION_NAME,
 * and AWS_REGION from the environment (falling back to the profile config).
 * Used by EKS IRSA, GitHub Actions OIDC, and other sts:AssumeRoleWithWebIdentity flows.
 * Returns nullptr if the required parameters can't be resolved.
 *
 * @param fallbackRegion Region to use when neither AWS_REGION nor
 *   AWS_DEFAULT_REGION is set — typically the ?region= from the S3 URL.
 *   Note: this also overrides any region from the profile config, since
 *   aws-c-auth gives options.region precedence over all implicit sources.
 *   Without this fallback the provider fails entirely in IRSA setups where
 *   the pod environment sets the token and role ARN but not the region
 *   (observed with AWS_CONFIG_FILE=/dev/null).
 */
std::shared_ptr<Aws::Crt::Auth::ICredentialsProvider> createSTSWebIdentityProvider(
    const std::string & profileName,
    const std::string & fallbackRegion,
    Aws::Crt::Io::ClientBootstrap * bootstrap,
    Aws::Crt::Io::TlsContext * tlsContext,
    Aws::Crt::Allocator * allocator = Aws::Crt::ApiAllocator())
{
    aws_credentials_provider_sts_web_identity_options options;
    AWS_ZERO_STRUCT(options);

    options.bootstrap = bootstrap->GetUnderlyingHandle();
    options.tls_ctx = tlsContext ? tlsContext->GetUnderlyingHandle() : nullptr;
    if (!profileName.empty()) {
        options.profile_name_override = aws_byte_cursor_from_c_str(profileName.c_str());
    }

    // aws-c-auth gives options.region precedence over env vars, so only set it
    // when env vars are absent — otherwise we'd mask a user-supplied AWS_REGION.
    if (!fallbackRegion.empty() && !awsRegionSetInEnv()) {
        options.region = aws_byte_cursor_from_c_str(fallbackRegion.c_str());
    }

    return createWrappedProvider(aws_credentials_provider_new_sts_web_identity(allocator, &options), allocator);
}

/**
 * Create an ECS container credentials provider using the C library directly.
 * This reads AWS_CONTAINER_CREDENTIALS_RELATIVE_URI or
 * AWS_CONTAINER_CREDENTIALS_FULL_URI (plus the optional
 * AWS_CONTAINER_AUTHORIZATION_TOKEN / _TOKEN_FILE) from the environment.
 * Used by ECS tasks and EKS Pod Identity.
 * Returns nullptr if neither URI env var is set.
 */
std::shared_ptr<Aws::Crt::Auth::ICredentialsProvider> createECSProvider(
    Aws::Crt::Io::ClientBootstrap * bootstrap,
    Aws::Crt::Io::TlsContext * tlsContext,
    Aws::Crt::Allocator * allocator = Aws::Crt::ApiAllocator())
{
    aws_credentials_provider_ecs_environment_options options;
    AWS_ZERO_STRUCT(options);

    options.bootstrap = bootstrap->GetUnderlyingHandle();
    options.tls_ctx = tlsContext ? tlsContext->GetUnderlyingHandle() : nullptr;

    return createWrappedProvider(aws_credentials_provider_new_ecs_from_environment(allocator, &options), allocator);
}

Result getCredentialsFromProvider(std::shared_ptr<Aws::Crt::Auth::ICredentialsProvider> provider)
{
    if (!provider || !provider->IsValid()) {
        return {.kind = Result::ProviderInvalid};
    }

    auto prom = std::make_shared<std::promise<Result>>();
    auto fut = prom->get_future();

    provider->GetCredentials([prom](std::shared_ptr<Aws::Crt::Auth::Credentials> credentials, int errorCode) {
        if (errorCode != 0 || !credentials) {
            prom->set_value({.kind = Result::AwsError, .errorCode = errorCode});
        } else {
            auto accessKeyId = Aws::Crt::ByteCursorToStringView(credentials->GetAccessKeyId());
            auto secretAccessKey = Aws::Crt::ByteCursorToStringView(credentials->GetSecretAccessKey());
            auto sessionToken = Aws::Crt::ByteCursorToStringView(credentials->GetSessionToken());

            std::optional<std::string> sessionTokenStr;
            if (!sessionToken.empty()) {
                sessionTokenStr = std::string(sessionToken.data(), sessionToken.size());
            }

            prom->set_value({
                .accessKeyId = std::string(accessKeyId.data(), accessKeyId.size()),
                .secretAccessKey = std::string(secretAccessKey.data(), secretAccessKey.size()),
                .sessionToken = sessionTokenStr,
            });
        }
    });

    // AWS CRT GetCredentials is asynchronous and only guarantees the callback will be
    // invoked if the initial call returns success. There's no documented timeout mechanism,
    // so we add a timeout to prevent indefinite hanging if the callback is never called.
    auto timeout = std::chrono::seconds(30);
    if (fut.wait_for(timeout) == std::future_status::timeout) {
        return {.kind = Result::Timeout, .errorCode = int(timeout.count())};
    }

    return fut.get();
}

class ProviderImpl : public Provider
{
public:
    ProviderImpl()
    {
        // Install custom logger that routes AWS CRT logs through Nix's logging infrastructure.
        // This ensures AWS logs respect Nix's verbosity settings and are formatted consistently.
        initialiseAwsLogger();

        // Create a shared TLS context for SSO, STS WebIdentity, and ECS providers (required for HTTPS)
        auto allocator = Aws::Crt::ApiAllocator();
        auto tlsCtxOptions = Aws::Crt::Io::TlsContextOptions::InitDefaultClient(allocator);
        tlsContext =
            std::make_shared<Aws::Crt::Io::TlsContext>(tlsCtxOptions, Aws::Crt::Io::TlsMode::CLIENT, allocator);
        if (!tlsContext || !*tlsContext) {
            host->warn(
                "failed to create TLS context for AWS credential providers; SSO, STS WebIdentity, and ECS container authentication will be unavailable");
            tlsContext = nullptr;
        }

        // Get bootstrap (lives as long as apiHandle)
        bootstrap = Aws::Crt::ApiHandle::GetOrCreateStaticDefaultClientBootstrap();
    }

    Aws::Crt::Io::ClientBootstrap * getBootstrap()
    {
        return bootstrap;
    }

    Result getCredentials(const std::string & profile, const std::string & region) override;

    void forget(const std::string & profile, const std::string & region) override
    {
        credentialProviderCache.erase({profile, region});
    }

    std::shared_ptr<Aws::Crt::Auth::ICredentialsProvider>
    createProviderForProfile(const std::string & profile, const std::string & region);

private:
    Aws::Crt::ApiHandle apiHandle;
    std::shared_ptr<Aws::Crt::Io::TlsContext> tlsContext;
    Aws::Crt::Io::ClientBootstrap * bootstrap;
    // Keyed by (profile, region). Region is part of the key because it is baked
    // into the STS WebIdentity provider at construction time; two S3 URLs with
    // different ?region= parameters need distinct chains.
    boost::
        concurrent_flat_map<std::pair<std::string, std::string>, std::shared_ptr<Aws::Crt::Auth::ICredentialsProvider>>
            credentialProviderCache;
};

std::shared_ptr<Aws::Crt::Auth::ICredentialsProvider>
ProviderImpl::createProviderForProfile(const std::string & profile, const std::string & region)
{
    // profileDisplayName is only used for debug logging - SDK uses its default profile
    // when ProfileNameOverride is not set
    std::string profileDisplayName = profile.empty() ? "(default)" : profile;

    host->debug(
        "[pid=" + std::to_string(getpid()) + "] creating new AWS credential provider for profile '"
        + profileDisplayName + "'");

    // Build a custom credential chain: Environment → SSO → Profile → STS WebIdentity → ECS → IMDS
    // This works for both default and named profiles, ensuring consistent behavior
    // including SSO support and proper TLS context for STS-based role assumption.
    Aws::Crt::Auth::CredentialsProviderChainConfig chainConfig;
    auto allocator = Aws::Crt::ApiAllocator();

    auto addProviderToChain = [&](std::string_view name, auto createProvider) -> bool {
        if (auto provider = createProvider()) {
            chainConfig.Providers.push_back(provider);
            host->debug(
                "Added AWS " + std::string(name) + " Credential Provider to chain for profile '" + profileDisplayName
                + "'");
            return true;
        }
        host->debug("Skipped AWS " + std::string(name) + " Credential Provider for profile '" + profileDisplayName + "'");
        return false;
    };

    // 1. Environment variables (highest priority)
    addProviderToChain("Environment", [&]() {
        return Aws::Crt::Auth::CredentialsProvider::CreateCredentialsProviderEnvironment(allocator);
    });

    // 2. SSO provider (try it, will fail gracefully if not configured)
    if (tlsContext) {
        addProviderToChain("SSO", [&]() { return createSSOProvider(profile, bootstrap, tlsContext.get(), allocator); });
    } else {
        host->debug(
            "Skipped AWS SSO Credential Provider for profile '" + profileDisplayName + "': TLS context unavailable");
    }

    // 3. Profile provider (for static credentials and role_arn/source_profile with STS)
    addProviderToChain("Profile", [&]() {
        Aws::Crt::Auth::CredentialsProviderProfileConfig profileConfig;
        profileConfig.Bootstrap = bootstrap;
        profileConfig.TlsContext = tlsContext.get();
        if (!profile.empty()) {
            profileConfig.ProfileNameOverride = Aws::Crt::ByteCursorFromCString(profile.c_str());
        }
        return Aws::Crt::Auth::CredentialsProvider::CreateCredentialsProviderProfile(profileConfig, allocator);
    });

    // 4. STS WebIdentity (AWS_WEB_IDENTITY_TOKEN_FILE + AWS_ROLE_ARN — EKS IRSA, GitHub Actions OIDC)
    // 5. ECS container metadata (AWS_CONTAINER_CREDENTIALS_RELATIVE_URI — ECS tasks, EKS Pod Identity)
    // ECS and IMDS are mutually exclusive per both the aws-c-auth default chain and the
    // pre-2.33 aws-sdk-cpp DefaultAWSCredentialsProviderChain: when container credential
    // env vars are set, IMDS is skipped so a transient ECS endpoint failure can't silently
    // fall through to the (typically broader) EC2 instance profile.
    bool ecsAdded = false;
    if (tlsContext) {
        addProviderToChain("STS WebIdentity", [&]() {
            return createSTSWebIdentityProvider(profile, region, bootstrap, tlsContext.get(), allocator);
        });
        ecsAdded =
            addProviderToChain("ECS", [&]() { return createECSProvider(bootstrap, tlsContext.get(), allocator); });
    } else {
        host->debug(
            "Skipped AWS STS WebIdentity and ECS Credential Providers for profile '" + profileDisplayName
            + "': TLS context unavailable");
    }

    // 6. IMDS provider (for EC2 instances, lowest priority) — only if ECS didn't claim the slot
    if (!ecsAdded) {
        addProviderToChain("IMDS", [&]() {
            Aws::Crt::Auth::CredentialsProviderImdsConfig imdsConfig;
            imdsConfig.Bootstrap = bootstrap;
            return Aws::Crt::Auth::CredentialsProvider::CreateCredentialsProviderImds(imdsConfig, allocator);
        });
    } else {
        host->debug(
            "Skipped AWS IMDS Credential Provider for profile '" + profileDisplayName
            + "': ECS provider is active (mutually exclusive)");
    }

    return Aws::Crt::Auth::CredentialsProvider::CreateCredentialsProviderChain(chainConfig, allocator);
}

Result ProviderImpl::getCredentials(const std::string & profile, const std::string & region)
{
    std::shared_ptr<Aws::Crt::Auth::ICredentialsProvider> provider;
    auto key = std::make_pair(profile, region);

    credentialProviderCache.try_emplace_and_cvisit(
        key,
        nullptr,
        [&](auto & kv) { provider = kv.second = createProviderForProfile(profile, region); },
        [&](const auto & kv) { provider = kv.second; });

    if (!provider) {
        credentialProviderCache.erase_if(key, [](const auto & kv) {
            [[maybe_unused]] auto [_, provider] = kv;
            return !provider;
        });

        return {.kind = Result::ProviderCreationFailed};
    }

    return getCredentialsFromProvider(provider);
}

} // anonymous namespace

} // namespace nix::aws_module

extern "C" __attribute__((visibility("default"))) nix::aws_module::Provider *
nixAwsMakeProvider(int abiVersion, const nix::aws_module::Host * host, nix::aws_module::Result * error)
{
    using namespace nix::aws_module;
    if (abiVersion != nix::aws_module::abiVersion) {
        error->kind = Result::AwsError;
        return nullptr;
    }
    nix::aws_module::host = host;
    auto provider = std::make_unique<ProviderImpl>();
    if (!provider->getBootstrap()) {
        error->kind = Result::BootstrapFailed;
        return nullptr;
    }
    return provider.release();
}

extern "C" __attribute__((visibility("default"))) const char * nixAwsErrorString(int errorCode)
{
    return aws_error_str(errorCode);
}
