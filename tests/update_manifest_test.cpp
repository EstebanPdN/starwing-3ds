#include "../platform/3ds/source/update_manifest.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace starfox::platform_3ds;
void require(bool value) { if (!value) std::abort(); }
std::string fixture(const std::string& name = "Starwing-3DS.cia") {
    return "{\"tag_name\":\"v1.1\",\"draft\":false,\"prerelease\":false,\"assets\":[{\"name\":\""
        + name + "\",\"browser_download_url\":\"https://github.com/EstebanPdN/starwing-3ds/releases/download/v1.1/"
        + name + "\",\"size\":4096,\"digest\":\"sha256:" + std::string(64, 'a') + "\"}]}";
}
void replace(std::string& s, const std::string& from, const std::string& to) {
    const auto at = s.find(from); require(at != std::string::npos); s.replace(at, from.size(), to);
}
int main(int argc, char** argv) {
    if (argc > 1) {
        std::ifstream file(argv[1]);
        const std::string data{std::istreambuf_iterator<char>(file), {}};
        for (bool homebrew : {false, true}) {
            const auto r = parse_update_release(data, homebrew); require(r.has_value());
            std::cout << r->version << ' ' << r->bytes << ' ' << r->sha256 << ' ' << r->url << '\n';
        }
        return 0;
    }
    require(update_is_newer("v1.10", "1.9"));
    require(!update_is_newer("v1.0.0", "1.0"));
    require(!update_is_newer("v0.99", "1.0"));
    for (const auto* s : {"", "v", "1.", "1.0-E", "1.0-E0", "1.0-E65536", "1.2.3.4", "65536", "../1.0", "1.0/other"})
        require(!update_version(s));
    require(update_is_newer("v1.1-E12", "1.1-E2"));
    require(update_is_newer("v1.1", "1.1-E99"));
    require(!update_is_newer("v1.1-E99", "1.1"));
    require(update_is_newer("v1.2-E1", "1.1"));
    auto experiment=fixture(); replace(experiment,"v1.1","v1.2-E2");
    replace(experiment,"download/v1.1/","download/v1.2-E2/");
    replace(experiment,"\"prerelease\":false","\"prerelease\":true");
    require(parse_update_release(experiment,false,true).has_value());
    require(!parse_update_release(experiment,false));
    auto newer=experiment; replace(newer,"v1.2-E2","v1.2-E10");
    replace(newer,"download/v1.2-E2/","download/v1.2-E10/");
    auto list="["+experiment+","+fixture()+","+newer+"]";
    require(parse_update_release(list,false,true)->version=="v1.2-E10");
    require(parse_update_release(list,false)->version=="v1.1");
    require(!parse_update_release("[]",false,true));
    auto notes=fixture(); replace(notes,"\"draft\":false", "\"body\":\"## Changelog\\n- Test.\",\"draft\":false");
    require(parse_update_release(notes,false)->notes=="## Changelog\n- Test.");
    require(parse_update_release(fixture(), false).has_value());
    require(parse_update_release(fixture("Starwing-3DS.3dsx"), true).has_value());
    require(!parse_update_release(fixture(), true));
    for (const auto& replacement : {
            std::pair{"\"draft\":false", "\"draft\":true"},
            std::pair{"\"prerelease\":false", "\"prerelease\":true"},
            std::pair{"\"size\":4096", "\"size\":0"},
            std::pair{"\"size\":4096", "\"size\":67108865"},
            std::pair{"sha256:", "sha512:"},
            std::pair{"https://github.com/", "http://github.com/"},
            std::pair{"EstebanPdN/starwing-3ds", "another/repository"},
            std::pair{"download/v1.1/", "download/v1.2/"}}) {
        auto json = fixture(); replace(json, replacement.first, replacement.second);
        require(!parse_update_release(json, false));
    }
    auto duplicate = fixture(); replace(duplicate, "\"draft\":false", "\"draft\":false,\"draft\":false");
    require(!parse_update_release(duplicate, false));
    auto duplicate_asset = fixture(); const auto first = duplicate_asset.find("[{"), last = duplicate_asset.find("}]");
    duplicate_asset.insert(last + 1, "," + duplicate_asset.substr(first + 1, last - first));
    require(!parse_update_release(duplicate_asset, false));
    require(!parse_update_release(std::string(2U * 1024U * 1024U + 1U, ' '), false));
    require(!parse_update_release("{\"draft\":false}", false));
    std::cout << "PASS: version ordering, channels, repository/tag binding, exact names, size, hashes, duplicates and bounds\n";
}
