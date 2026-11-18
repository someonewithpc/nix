#include <regex>
#include <optional>
#include <string>

#include "nix/flake/url-name.hh"
#include "nix/util/strings.hh"
#include "nix/util/url.hh"

namespace nix {

static const std::string attributeNamePattern("[a-zA-Z0-9_-]+");
static const std::string pathSegmentPattern("[a-zA-Z0-9_-]+");

namespace {

struct UrlNameRegexes
{
    std::regex lastAttribute{"^((?:" + attributeNamePattern + "\\.)*)(" + attributeNamePattern + ")(\\^.*)?$"};
    std::regex lastPathSegment{".*/(" + pathSegmentPattern + ")"};
    std::regex secondPathSegment{"(?:" + pathSegmentPattern + ")/(" + pathSegmentPattern + ")(?:/.*)?"};
    std::regex gitProvider{"github|gitlab|sourcehut"};
    std::regex gitScheme{"git($|\\+.*)"};
};

} // namespace

std::optional<std::string> getNameFromURL(const ParsedURL & url)
{
    static const UrlNameRegexes re;
    std::smatch match;

    /* If there is a dir= argument, use its value */
    if (url.query.count("dir") > 0)
        return url.query.at("dir");

    /* If the fragment isn't a "default" and contains two attribute elements, use the last one */
    if (std::regex_match(url.fragment, match, re.lastAttribute) && match.str(1) != "defaultPackage."
        && match.str(2) != "default") {
        return match.str(2);
    }

    /* This is not right, because special chars like slashes within the
       path fragments should be percent encoded, but I don't think any
       of the regexes above care. */
    auto path = concatStringsSep("/", url.path);

    /* If this is a github/gitlab/sourcehut flake, use the repo name */
    if (std::regex_match(url.scheme, re.gitProvider) && std::regex_match(path, match, re.secondPathSegment))
        return match.str(1);

    /* If it is a regular git flake, use the directory name */
    if (std::regex_match(url.scheme, re.gitScheme) && std::regex_match(path, match, re.lastPathSegment))
        return match.str(1);

    /* If there is no fragment, take the last element of the path */
    if (std::regex_match(path, match, re.lastPathSegment))
        return match.str(1);

    /* If even that didn't work, the URL does not contain enough info to determine a useful name */
    return {};
}

} // namespace nix
