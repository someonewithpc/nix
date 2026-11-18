#pragma once
///@file

/* The interface between libnixstore and the module that holds the AWS
   CRT code, which libnixstore loads the first time it needs AWS
   credentials, or links in a fully static build. The module links no Nix library, so that it works the
   same whether the Nix libraries are shared or linked into the
   executable, and only reaches Nix through the host callbacks */

#include <optional>
#include <string>

namespace nix::aws_module {

constexpr int abiVersion = 1;

struct Host
{
    /* The AWS log level that the current verbosity lets through */
    int (*getLogLevel)();
    /* Log a message at an AWS log level */
    void (*log)(int awsLevel, const char * subject, const char * message);
    void (*debug)(const std::string & message);
    void (*warn)(const std::string & message);
};

struct Result
{
    enum Kind {
        Ok,
        /* errorCode is an AWS error code */
        AwsError,
        ProviderInvalid,
        /* errorCode is the timeout in seconds */
        Timeout,
        ProviderCreationFailed,
        BootstrapFailed,
    } kind = Ok;

    int errorCode = 0;
    std::string accessKeyId;
    std::string secretAccessKey;
    std::optional<std::string> sessionToken;
};

struct Provider
{
    virtual Result getCredentials(const std::string & profile, const std::string & region) = 0;
    /* Forget the cached provider chain for a profile and region */
    virtual void forget(const std::string & profile, const std::string & region) = 0;
    virtual ~Provider() = default;
};

/* The module's entry points. nixAwsMakeProvider returns null with
   *error set when the provider can't be set up, and on an ABI mismatch */
using MakeProvider = Provider * (*) (int abiVersion, const Host * host, Result * error);
using ErrorString = const char * (*) (int errorCode);

} // namespace nix::aws_module

extern "C" nix::aws_module::Provider *
nixAwsMakeProvider(int abiVersion, const nix::aws_module::Host * host, nix::aws_module::Result * error);
extern "C" const char * nixAwsErrorString(int errorCode);
