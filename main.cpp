
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>
#include <objbase.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include "external/imgui/imgui.h"
#include "external/imgui/backends/imgui_impl_sdl3.h"
#include "external/imgui/backends/imgui_impl_opengl3.h"
#include "external/json/json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ============================================================
// DATA
// ============================================================

struct Mod
{
    std::string name;
    std::string description;
    std::string version;
    std::string file;
};

struct Modpack
{
    std::string id;
    std::string name;
    std::string description;
    std::string version;
    std::string file;
    std::vector<Mod> mods;
};

struct Repository
{
    std::string url;
    std::string owner;
    std::string name;
    std::string branch = "main";

    std::vector<Mod> mods;
    std::vector<Modpack> modpacks;

    bool loaded = false;
    std::string loadError;
};

struct InstalledMod
{
    std::string name;
    std::string description;
    std::string version;
    std::string file;

    std::string repoURL;
    std::string repoLabel;

    bool enabled = true;

    // Mods installed by packs are only removed when no pack owns them
    // and the user did not also install them manually.
    bool manuallyInstalled = true;
    std::vector<std::string> packOwners;
};

struct InstalledPack
{
    std::string id;
    std::string name;
    std::string description;
    std::string version;
    std::string repoURL;
    std::string repoLabel;
    std::vector<std::string> files;
};

struct Profile
{
    std::string id;
    std::string name;
    std::vector<InstalledMod> mods;
    std::vector<InstalledPack> packs;
};

// ============================================================
// GLOBALS
// ============================================================

static char repoInput[512] = {};
static char profileNameInput[128] = {};

static std::string modsFolder;

static std::vector<Repository> repositories;
static std::vector<Profile> profiles;

static int selectedRepository = -1;
static int selectedProfile = -1;
static int activeProfile = -1;

static std::string status = "Welcome to CyzonVSModManager.";
static std::string error;

// ============================================================
// UTF CONVERSION
// ============================================================

std::wstring ToWide(const std::string& value)
{
    if (value.empty())
        return {};

    int size = MultiByteToWideChar(
        CP_UTF8, 0, value.c_str(), -1, nullptr, 0
    );

    if (size <= 0)
        return {};

    std::wstring result(size, L'\0');

    MultiByteToWideChar(
        CP_UTF8, 0, value.c_str(), -1,
        result.data(), size
    );

    result.pop_back();
    return result;
}

std::string ToUTF8(const std::wstring& value)
{
    if (value.empty())
        return {};

    int size = WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), -1,
        nullptr, 0, nullptr, nullptr
    );

    if (size <= 0)
        return {};

    std::string result(size, '\0');

    WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), -1,
        result.data(), size, nullptr, nullptr
    );

    result.pop_back();
    return result;
}

// ============================================================
// PROFILE HELPERS
// ============================================================

Profile* CurrentProfile()
{
    if (
        activeProfile < 0 ||
        activeProfile >= static_cast<int>(profiles.size())
    )
        return nullptr;

    return &profiles[activeProfile];
}

const Profile* CurrentProfileConst()
{
    if (
        activeProfile < 0 ||
        activeProfile >= static_cast<int>(profiles.size())
    )
        return nullptr;

    return &profiles[activeProfile];
}

fs::path ProfilesRoot()
{
    return fs::u8path("profiles");
}

fs::path ProfilePath(const Profile& profile)
{
    return ProfilesRoot() / fs::u8path(profile.id);
}

fs::path ProfileModsPath(const Profile& profile)
{
    return ProfilePath(profile) / "mods";
}

fs::path ProfileDisabledPath(const Profile& profile)
{
    return ProfilePath(profile) / "disabled";
}

fs::path ProfileDatabasePath(const Profile& profile)
{
    return ProfilePath(profile) / "profile.json";
}

fs::path ProfileModPath(
    const Profile& profile,
    const InstalledMod& mod
)
{
    return (
        mod.enabled
        ? ProfileModsPath(profile)
        : ProfileDisabledPath(profile)
    ) / fs::u8path(mod.file);
}

InstalledMod* FindInstalled(const std::string& filename)
{
    Profile* profile = CurrentProfile();

    if (!profile)
        return nullptr;

    for (auto& mod : profile->mods)
    {
        if (mod.file == filename)
            return &mod;
    }

    return nullptr;
}

const InstalledMod* FindInstalledConst(
    const std::string& filename
)
{
    const Profile* profile = CurrentProfileConst();

    if (!profile)
        return nullptr;

    for (const auto& mod : profile->mods)
    {
        if (mod.file == filename)
            return &mod;
    }

    return nullptr;
}

// ============================================================
// HTTP GET
// ============================================================

bool HTTPGet(
    const std::wstring& url,
    std::string& output,
    std::string& errorMessage
)
{
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);

    parts.dwSchemeLength = DWORD(-1);
    parts.dwHostNameLength = DWORD(-1);
    parts.dwUrlPathLength = DWORD(-1);
    parts.dwExtraInfoLength = DWORD(-1);

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
    {
        errorMessage = "Invalid URL.";
        return false;
    }

    std::wstring host(
        parts.lpszHostName,
        parts.dwHostNameLength
    );

    std::wstring path(
        parts.lpszUrlPath,
        parts.dwUrlPathLength
    );

    if (parts.dwExtraInfoLength)
    {
        path.append(
            parts.lpszExtraInfo,
            parts.dwExtraInfoLength
        );
    }

    HINTERNET session = WinHttpOpen(
        L"CyzonVSModManager/3.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );

    if (!session)
    {
        errorMessage = "Could not initialize WinHTTP.";
        return false;
    }

    HINTERNET connection = WinHttpConnect(
        session,
        host.c_str(),
        parts.nPort,
        0
    );

    if (!connection)
    {
        errorMessage = "Could not connect to GitHub.";
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD flags =
        parts.nScheme == INTERNET_SCHEME_HTTPS
        ? WINHTTP_FLAG_SECURE : 0;

    HINTERNET request = WinHttpOpenRequest(
        connection,
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags
    );

    if (!request)
    {
        errorMessage = "Could not create HTTP request.";
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    WinHttpAddRequestHeaders(
        request,
        L"User-Agent: CyzonVSModManager/3.0\r\n",
        DWORD(-1),
        WINHTTP_ADDREQ_FLAG_ADD
    );

    bool success = false;

    if (
        WinHttpSendRequest(
            request,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0
        )
        &&
        WinHttpReceiveResponse(request, nullptr)
    )
    {
        DWORD statusCode = 0;
        DWORD size = sizeof(statusCode);

        if (WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE |
            WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode,
            &size,
            WINHTTP_NO_HEADER_INDEX
        ))
        {
            if (statusCode >= 200 && statusCode < 300)
            {
                output.clear();
                success = true;

                while (true)
                {
                    DWORD available = 0;

                    if (!WinHttpQueryDataAvailable(
                        request, &available
                    ))
                    {
                        errorMessage =
                            "Failed checking downloaded data.";

                        success = false;
                        break;
                    }

                    if (available == 0)
                        break;

                    std::vector<char> buffer(available);
                    DWORD downloaded = 0;

                    if (!WinHttpReadData(
                        request,
                        buffer.data(),
                        available,
                        &downloaded
                    ))
                    {
                        errorMessage =
                            "Failed reading downloaded data.";

                        success = false;
                        break;
                    }

                    if (downloaded == 0)
                        break;

                    output.append(
                        buffer.data(),
                        downloaded
                    );
                }
            }
            else
            {
                errorMessage =
                    "HTTP Error: " +
                    std::to_string(statusCode);
            }
        }
        else
        {
            errorMessage = "Could not read HTTP status code.";
        }
    }
    else
    {
        errorMessage = "HTTP request failed.";
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    return success;
}

// ============================================================
// GITHUB REPOSITORIES
// ============================================================

bool ParseRepositoryURL(
    std::string url,
    std::string& owner,
    std::string& repository
)
{
    const std::string prefix = "https://github.com/";

    if (url.rfind(prefix, 0) != 0)
        return false;

    url = url.substr(prefix.size());

    while (!url.empty() && url.back() == '/')
        url.pop_back();

    if (
        url.size() > 4 &&
        url.substr(url.size() - 4) == ".git"
    )
    {
        url.resize(url.size() - 4);
    }

    size_t slash = url.find('/');

    if (
        slash == std::string::npos ||
        slash == 0 ||
        slash == url.size() - 1 ||
        url.find('/', slash + 1) != std::string::npos
    )
        return false;

    owner = url.substr(0, slash);
    repository = url.substr(slash + 1);

    return true;
}

std::string RepositoryLabel(const Repository& repo)
{
    return repo.owner + "/" + repo.name;
}

std::string MakeRawURL(
    const Repository& repo,
    const std::string& file
)
{
    return
        "https://raw.githubusercontent.com/" +
        repo.owner + "/" +
        repo.name +
        "/refs/heads/" +
        repo.branch + "/" +
        file;
}

bool IsValidDLL(const std::string& filename)
{
    if (
        filename.empty() ||
        filename.find("..") != std::string::npos ||
        filename.find('/') != std::string::npos ||
        filename.find('\\') != std::string::npos ||
        filename.find(':') != std::string::npos
    )
        return false;

    if (filename.size() < 5)
        return false;

    std::string extension =
        filename.substr(filename.size() - 4);

    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char c)
        {
            return static_cast<char>(std::tolower(c));
        }
    );

    return extension == ".dll";
}

int CompareVersions(const std::string& a, const std::string& b);

bool LoadRepository(Repository& repo)
{
    std::string owner;
    std::string name;

    if (!ParseRepositoryURL(repo.url, owner, name))
    {
        repo.loadError = "Invalid GitHub repository URL.";
        repo.loaded = false;
        return false;
    }

    repo.owner = owner;
    repo.name = name;
    repo.mods.clear();
    repo.modpacks.clear();
    repo.loadError.clear();

    status = "Connecting to " + RepositoryLabel(repo) + "...";

    std::string response;
    std::string requestError;

    std::string apiURL =
        "https://api.github.com/repos/" +
        owner + "/" + name;

    if (!HTTPGet(ToWide(apiURL), response, requestError))
    {
        repo.loadError = requestError;
        repo.loaded = false;
        return false;
    }

    try
    {
        json info = json::parse(response);

        repo.branch = info.value(
            "default_branch",
            std::string("main")
        );
    }
    catch (...)
    {
        repo.loadError =
            "Could not parse repository information.";

        repo.loaded = false;
        return false;
    }

    std::string manifest;

    if (!HTTPGet(
        ToWide(MakeRawURL(repo, "mods.json")),
        manifest,
        requestError
    ))
    {
        repo.loadError =
            "Could not download mods.json: " +
            requestError;

        repo.loaded = false;
        return false;
    }

    try
    {
        json data = json::parse(manifest);

        if (
            !data.contains("mods") ||
            !data["mods"].is_array()
        )
        {
            repo.loadError =
                "mods.json must contain a mods array.";

            repo.loaded = false;
            return false;
        }

        for (const auto& item : data["mods"])
        {
            if (!item.is_object())
                continue;

            Mod mod;

            mod.name = item.value("name", std::string{});
            mod.description = item.value("description", std::string{});
            mod.version = item.value("version", std::string{});
            mod.file = item.value("file", std::string{});

            if (
                mod.name.empty() ||
                mod.version.empty() ||
                !IsValidDLL(mod.file)
            )
                continue;

            bool duplicate = false;

            for (const auto& existing : repo.mods)
            {
                if (existing.file == mod.file)
                {
                    duplicate = true;
                    break;
                }
            }

            if (!duplicate)
                repo.mods.push_back(mod);
        }
    }
    catch (...)
    {
        repo.loadError =
            "Could not parse mods.json.";

        repo.loaded = false;
        return false;
    }

    // Modpacks are optional. Repositories without modpacks.json
    // continue to work exactly like ordinary mod repositories.
    std::string packsManifest;
    std::string packsError;

    if (HTTPGet(
        ToWide(MakeRawURL(repo, "modpacks.json")),
        packsManifest,
        packsError
    ))
    {
        try
        {
            json packsData = json::parse(packsManifest);

            if (
                packsData.contains("modpacks") &&
                packsData["modpacks"].is_array()
            )
            {
                for (const auto& packInfo : packsData["modpacks"])
                {
                    if (!packInfo.is_object())
                        continue;

                    Modpack pack;
                    pack.id = packInfo.value("id", std::string{});
                    pack.name = packInfo.value("name", std::string{});
                    pack.description = packInfo.value("description", std::string{});
                    pack.version = packInfo.value("version", std::string{});
                    pack.file = packInfo.value("file", std::string{});

                    // Pack definition files must remain inside modpacks/.
                    if (
                        pack.id.empty() ||
                        pack.name.empty() ||
                        pack.version.empty() ||
                        pack.file.rfind("modpacks/", 0) != 0 ||
                        pack.file.find("..") != std::string::npos ||
                        pack.file.find('\\') != std::string::npos ||
                        pack.file.find(':') != std::string::npos ||
                        pack.file.find('?') != std::string::npos ||
                        pack.file.find('#') != std::string::npos
                    )
                        continue;

                    std::string packJson;
                    std::string packError;

                    if (!HTTPGet(
                        ToWide(MakeRawURL(repo, pack.file)),
                        packJson,
                        packError
                    ))
                        continue;

                    try
                    {
                        json definition = json::parse(packJson);

                        if (
                            !definition.contains("mods") ||
                            !definition["mods"].is_array()
                        )
                            continue;

                        // The definition can override display metadata,
                        // while the repository manifest remains the source
                        // of truth for actual DLL paths and versions.
                        pack.name = definition.value("name", pack.name);
                        pack.description = definition.value("description", pack.description);
                        pack.version = definition.value("version", pack.version);

                        for (const auto& entry : definition["mods"])
                        {
                            if (!entry.is_object())
                                continue;

                            std::string filename =
                                entry.value("file", std::string{});

                            if (!IsValidDLL(filename))
                                continue;

                            auto found = std::find_if(
                                repo.mods.begin(),
                                repo.mods.end(),
                                [&](const Mod& candidate)
                                {
                                    return candidate.file == filename;
                                }
                            );

                            if (found == repo.mods.end())
                                continue;

                            std::string requiredVersion =
                                entry.value("version", std::string{});

                            if (
                                !requiredVersion.empty() &&
                                CompareVersions(found->version, requiredVersion) < 0
                            )
                                continue;

                            bool duplicate = std::any_of(
                                pack.mods.begin(),
                                pack.mods.end(),
                                [&](const Mod& existing)
                                {
                                    return existing.file == filename;
                                }
                            );

                            if (!duplicate)
                                pack.mods.push_back(*found);
                        }

                        if (!pack.mods.empty())
                            repo.modpacks.push_back(std::move(pack));
                    }
                    catch (...)
                    {
                        // Ignore an invalid individual pack without
                        // preventing the repository's mods from loading.
                    }
                }
            }
        }
        catch (...)
        {
            // A malformed optional modpacks.json must not break mods.json.
        }
    }

    repo.loaded = true;

    status =
        "Loaded " +
        std::to_string(repo.mods.size()) +
        " mods and " +
        std::to_string(repo.modpacks.size()) +
        " modpacks from " +
        RepositoryLabel(repo) + ".";

    return true;
}

bool AddRepository(const std::string& url)
{
    std::string owner;
    std::string name;

    if (!ParseRepositoryURL(url, owner, name))
    {
        error =
            "Use https://github.com/owner/repository";

        return false;
    }

    for (size_t i = 0; i < repositories.size(); ++i)
    {
        if (
            repositories[i].owner == owner &&
            repositories[i].name == name
        )
        {
            selectedRepository = static_cast<int>(i);

            std::snprintf(
                repoInput,
                sizeof(repoInput),
                "%s",
                repositories[i].url.c_str()
            );

            status = "Repository already added.";
            error.clear();
            return true;
        }
    }

    Repository repo;
    repo.url = "https://github.com/" + owner + "/" + name;

    if (!LoadRepository(repo))
    {
        error = repo.loadError;
        return false;
    }

    repositories.push_back(repo);

    selectedRepository =
        static_cast<int>(repositories.size()) - 1;

    std::snprintf(
        repoInput,
        sizeof(repoInput),
        "%s",
        repositories.back().url.c_str()
    );

    error.clear();
    return true;
}

// ============================================================
// PROFILE DATABASE
// ============================================================

void SaveProfile(Profile& profile)
{
    std::error_code ec;

    fs::create_directories(
        ProfilePath(profile),
        ec
    );

    json data;
    data["name"] = profile.name;
    data["mods"] = json::array();

    for (const auto& mod : profile.mods)
    {
        data["mods"].push_back({
            {"name", mod.name},
            {"description", mod.description},
            {"version", mod.version},
            {"file", mod.file},
            {"repo_url", mod.repoURL},
            {"repo_label", mod.repoLabel},
            {"enabled", mod.enabled},
            {"manually_installed", mod.manuallyInstalled},
            {"pack_owners", mod.packOwners}
        });
    }

    data["packs"] = json::array();

    for (const auto& pack : profile.packs)
    {
        data["packs"].push_back({
            {"id", pack.id},
            {"name", pack.name},
            {"description", pack.description},
            {"version", pack.version},
            {"repo_url", pack.repoURL},
            {"repo_label", pack.repoLabel},
            {"files", pack.files}
        });
    }

    std::ofstream file(
        ProfileDatabasePath(profile),
        std::ios::binary
    );

    if (!file)
    {
        error =
            "Could not save profile: " +
            profile.name;

        return;
    }

    file << data.dump(4);
}

void LoadProfile(Profile& profile)
{
    profile.mods.clear();
    profile.packs.clear();

    std::ifstream file(
        ProfileDatabasePath(profile),
        std::ios::binary
    );

    if (!file)
        return;

    try
    {
        json data = json::parse(file);

        profile.name = data.value(
            "name",
            profile.name
        );

        if (
            !data.contains("mods") ||
            !data["mods"].is_array()
        )
            return;

        for (const auto& item : data["mods"])
        {
            if (!item.is_object())
                continue;

            InstalledMod mod;

            mod.name = item.value("name", std::string{});
            mod.description = item.value("description", std::string{});
            mod.version = item.value("version", std::string{});
            mod.file = item.value("file", std::string{});

            mod.repoURL = item.value("repo_url", std::string{});
            mod.repoLabel = item.value("repo_label", std::string{});

            mod.enabled = item.value("enabled", true);
            mod.manuallyInstalled = item.value("manually_installed", true);
            mod.packOwners = item.value(
                "pack_owners",
                std::vector<std::string>{}
            );

            if (
                IsValidDLL(mod.file) &&
                !mod.repoURL.empty()
            )
            {
                profile.mods.push_back(mod);
            }
        }

        if (data.contains("packs") && data["packs"].is_array())
        {
            for (const auto& item : data["packs"])
            {
                if (!item.is_object())
                    continue;

                InstalledPack pack;
                pack.id = item.value("id", std::string{});
                pack.name = item.value("name", std::string{});
                pack.description = item.value("description", std::string{});
                pack.version = item.value("version", std::string{});
                pack.repoURL = item.value("repo_url", std::string{});
                pack.repoLabel = item.value("repo_label", std::string{});
                pack.files = item.value(
                    "files",
                    std::vector<std::string>{}
                );

                if (!pack.id.empty() && !pack.repoURL.empty())
                    profile.packs.push_back(std::move(pack));
            }
        }
    }
    catch (...)
    {
        error =
            "Could not load profile: " +
            profile.name;
    }
}

void SaveAllProfiles()
{
    for (auto& profile : profiles)
        SaveProfile(profile);
}

// ============================================================
// SETTINGS
// ============================================================

void SaveSettings()
{
    std::ofstream file("cyzon_settings.ini");

    if (!file)
    {
        error = "Could not save settings.";
        return;
    }

    file << "repo_count=" << repositories.size() << '\n';
    file << "selected_repo=" << selectedRepository << '\n';
    file << "mods_folder=" << modsFolder << '\n';

    for (size_t i = 0; i < repositories.size(); ++i)
    {
        file
            << "repo_" << i
            << "="
            << repositories[i].url
            << '\n';
    }

    file << "profile_count=" << profiles.size() << '\n';

    file
        << "active_profile="
        << (
            activeProfile >= 0 &&
            activeProfile < static_cast<int>(profiles.size())
            ? profiles[activeProfile].id
            : ""
        )
        << '\n';

    for (size_t i = 0; i < profiles.size(); ++i)
    {
        file
            << "profile_" << i << "_id="
            << profiles[i].id
            << '\n';

        file
            << "profile_" << i << "_name="
            << profiles[i].name
            << '\n';
    }
}

void LoadSettings()
{
    std::ifstream file("cyzon_settings.ini");

    std::string line;
    std::string savedActiveProfile;

    std::vector<std::pair<int, std::string>> savedRepos;
    std::vector<std::pair<int, Profile>> savedProfiles;

    while (std::getline(file, line))
    {
        size_t separator = line.find('=');

        if (separator == std::string::npos)
            continue;

        std::string key = line.substr(0, separator);
        std::string value = line.substr(separator + 1);

        if (key == "mods_folder")
        {
            modsFolder = value;
        }
        else if (key == "active_profile")
        {
            savedActiveProfile = value;
        }
        else if (key == "repo")
        {
            // Old settings compatibility.
            if (!value.empty())
                savedRepos.push_back({0, value});
        }
        else if (
            key.rfind("repo_", 0) == 0 &&
            key != "repo_count"
        )
        {
            int index = std::atoi(key.substr(5).c_str());

            if (!value.empty())
                savedRepos.push_back({index, value});
        }
        else if (key.rfind("profile_", 0) == 0)
        {
            size_t idPosition = key.find("_id");
            size_t namePosition = key.find("_name");

            if (idPosition != std::string::npos)
            {
                int index = std::atoi(
                    key.substr(8, idPosition - 8).c_str()
                );

                if (index >= 0)
                {
                    if (savedProfiles.size() <= static_cast<size_t>(index))
                        savedProfiles.resize(index + 1);

                    savedProfiles[index].first = index;
                    savedProfiles[index].second.id = value;
                }
            }
            else if (namePosition != std::string::npos)
            {
                int index = std::atoi(
                    key.substr(8, namePosition - 8).c_str()
                );

                if (index >= 0)
                {
                    if (savedProfiles.size() <= static_cast<size_t>(index))
                        savedProfiles.resize(index + 1);

                    savedProfiles[index].first = index;
                    savedProfiles[index].second.name = value;
                }
            }
        }
    }

    std::sort(
        savedRepos.begin(),
        savedRepos.end(),
        [](const auto& a, const auto& b)
        {
            return a.first < b.first;
        }
    );

    repositories.clear();

    for (const auto& entry : savedRepos)
    {
        std::string owner;
        std::string name;

        if (!ParseRepositoryURL(entry.second, owner, name))
            continue;

        bool duplicate = false;

        for (const auto& existing : repositories)
        {
            if (
                existing.owner == owner &&
                existing.name == name
            )
            {
                duplicate = true;
                break;
            }
        }

        if (!duplicate)
        {
            Repository repo;
            repo.url =
                "https://github.com/" + owner + "/" + name;

            repositories.push_back(repo);
        }
    }

    selectedRepository =
        repositories.empty() ? -1 : 0;

    profiles.clear();

    for (const auto& entry : savedProfiles)
    {
        Profile profile = entry.second;

        if (
            !profile.id.empty() &&
            !profile.name.empty()
        )
        {
            profiles.push_back(profile);
        }
    }

    for (auto& profile : profiles)
        LoadProfile(profile);

    activeProfile = -1;

    for (size_t i = 0; i < profiles.size(); ++i)
    {
        if (profiles[i].id == savedActiveProfile)
        {
            activeProfile = static_cast<int>(i);
            break;
        }
    }

    if (activeProfile < 0 && !profiles.empty())
        activeProfile = 0;

    selectedProfile = activeProfile;

    if (selectedRepository >= 0)
    {
        std::snprintf(
            repoInput,
            sizeof(repoInput),
            "%s",
            repositories[selectedRepository].url.c_str()
        );
    }
}

// ============================================================
// PROFILE CREATION
// ============================================================

std::string MakeProfileID()
{
    int number = 1;

    while (true)
    {
        std::string id =
            "profile_" + std::to_string(number);

        bool exists = false;

        for (const auto& profile : profiles)
        {
            if (profile.id == id)
            {
                exists = true;
                break;
            }
        }

        if (!exists)
            return id;

        ++number;
    }
}

bool CreateProfile(const std::string& name)
{
    if (name.empty())
    {
        error = "Enter a profile name.";
        return false;
    }

    for (const auto& profile : profiles)
    {
        if (_stricmp(profile.name.c_str(), name.c_str()) == 0)
        {
            error = "A profile with that name already exists.";
            return false;
        }
    }

    Profile profile;

    profile.id = MakeProfileID();
    profile.name = name;

    std::error_code ec;

    fs::create_directories(
        ProfileModsPath(profile),
        ec
    );

    fs::create_directories(
        ProfileDisabledPath(profile),
        ec
    );

    profiles.push_back(profile);

    selectedProfile =
        static_cast<int>(profiles.size()) - 1;

    SaveProfile(profiles.back());
    SaveSettings();

    status = "Created profile: " + name;
    error.clear();

    return true;
}

// ============================================================
// PROFILE FILE SYNC
// ============================================================

bool CopyFileSafely(
    const fs::path& source,
    const fs::path& destination,
    std::string& message
)
{
    std::error_code ec;

    if (!fs::exists(source, ec))
    {
        message =
            "Missing profile DLL: " +
            source.string();

        return false;
    }

    fs::create_directories(
        destination.parent_path(),
        ec
    );

    if (ec)
    {
        message =
            "Could not create destination directory: " +
            ec.message();

        return false;
    }

    fs::path temporary = destination;
    temporary += ".cyzon-temp";

    fs::copy_file(
        source,
        temporary,
        fs::copy_options::overwrite_existing,
        ec
    );

    if (ec)
    {
        message =
            "Could not copy DLL: " +
            ec.message();

        return false;
    }

    // MoveFileEx replaces the old destination only after
    // the new copy has been written successfully.

    if (!MoveFileExW(
        temporary.c_str(),
        destination.c_str(),
        MOVEFILE_REPLACE_EXISTING |
        MOVEFILE_WRITE_THROUGH
    ))
    {
        DWORD code = GetLastError();

        fs::remove(temporary, ec);

        message =
            "Could not replace DLL. Windows error: " +
            std::to_string(code);

        return false;
    }

    return true;
}

bool RemoveActiveProfileFiles(
    const Profile& profile,
    std::string& message
)
{
    if (modsFolder.empty())
    {
        message = "Select the game mods folder first.";
        return false;
    }

    std::error_code ec;

    for (const auto& mod : profile.mods)
    {
        if (!mod.enabled)
            continue;

        fs::path activePath =
            fs::u8path(modsFolder) / fs::u8path(mod.file);

        if (!fs::exists(activePath, ec))
        {
            ec.clear();
            continue;
        }

        if (!fs::remove(activePath, ec) || ec)
        {
            message =
                "Could not remove active DLL " +
                mod.file +
                ". Close the game and try again.";

            return false;
        }
    }

    return true;
}

bool ActivateProfileFiles(
    const Profile& profile,
    std::string& message
)
{
    if (modsFolder.empty())
    {
        message = "Select the game mods folder first.";
        return false;
    }

    std::error_code ec;

    fs::create_directories(
        fs::u8path(modsFolder),
        ec
    );

    if (ec)
    {
        message =
            "Could not access game mods folder: " +
            ec.message();

        return false;
    }

    // Preflight: make sure all target files exist and
    // no untracked files would be overwritten.

    const Profile* oldProfile = CurrentProfileConst();

    for (const auto& mod : profile.mods)
    {
        if (!mod.enabled)
            continue;

        fs::path source =
            ProfileModsPath(profile) / fs::u8path(mod.file);

        if (!fs::exists(source, ec))
        {
            message =
                "Profile is missing " +
                mod.file;

            return false;
        }

        fs::path destination =
            fs::u8path(modsFolder) / fs::u8path(mod.file);

        if (fs::exists(destination, ec))
        {
            bool ownedByOldProfile = false;

            if (oldProfile)
            {
                for (const auto& oldMod : oldProfile->mods)
                {
                    if (
                        oldMod.enabled &&
                        oldMod.file == mod.file
                    )
                    {
                        ownedByOldProfile = true;
                        break;
                    }
                }
            }

            if (!ownedByOldProfile)
            {
                message =
                    "Cannot activate profile. An unmanaged DLL "
                    "already exists: " + mod.file;

                return false;
            }
        }
    }

    // Copy all target files.

    std::vector<fs::path> copiedFiles;

    for (const auto& mod : profile.mods)
    {
        if (!mod.enabled)
            continue;

        fs::path source =
            ProfileModsPath(profile) / fs::u8path(mod.file);

        fs::path destination =
            fs::u8path(modsFolder) / fs::u8path(mod.file);

        if (!CopyFileSafely(source, destination, message))
        {
            // Remove files already copied from this profile.
            for (const auto& path : copiedFiles)
            {
                std::error_code cleanup;
                fs::remove(path, cleanup);
            }

            return false;
        }

        copiedFiles.push_back(destination);
    }

    return true;
}

// ============================================================
// SWITCH PROFILE
// ============================================================

bool SwitchProfile(int newIndex)
{
    if (
        newIndex < 0 ||
        newIndex >= static_cast<int>(profiles.size())
    )
        return false;

    if (newIndex == activeProfile)
        return true;

    if (modsFolder.empty())
    {
        error = "Select the game mods folder first.";
        return false;
    }

    std::string message;

    Profile* oldProfile = CurrentProfile();

    // Check target files before removing anything from
    // the currently active profile.

    const Profile& target = profiles[newIndex];

    std::error_code ec;

    for (const auto& mod : target.mods)
    {
        if (!mod.enabled)
            continue;

        fs::path source =
            ProfileModsPath(target) / fs::u8path(mod.file);

        if (!fs::exists(source, ec))
        {
            error =
                "Target profile is missing " +
                mod.file;

            return false;
        }

        fs::path destination =
            fs::u8path(modsFolder) / fs::u8path(mod.file);

        if (fs::exists(destination, ec))
        {
            bool oldOwnsFile = false;

            if (oldProfile)
            {
                for (const auto& oldMod : oldProfile->mods)
                {
                    if (
                        oldMod.enabled &&
                        oldMod.file == mod.file
                    )
                    {
                        oldOwnsFile = true;
                        break;
                    }
                }
            }

            if (!oldOwnsFile)
            {
                error =
                    "A DLL not managed by the current profile "
                    "would be overwritten: " + mod.file;

                return false;
            }
        }
    }

    // Remove currently active profile files.

    if (oldProfile)
    {
        if (!RemoveActiveProfileFiles(*oldProfile, message))
        {
            error = message;
            return false;
        }
    }

    // Activate new profile.

    if (!ActivateProfileFiles(target, message))
    {
        // Try to restore the old profile if activation fails.

        if (oldProfile)
        {
            std::string restoreError;

            ActivateProfileFiles(
                *oldProfile,
                restoreError
            );
        }

        error = message;
        return false;
    }

    activeProfile = newIndex;
    selectedProfile = newIndex;

    SaveSettings();

    status =
        "Switched to profile: " +
        profiles[activeProfile].name;

    error.clear();

    return true;
}

// ============================================================
// DELETE PROFILE
// ============================================================

bool DeleteProfile(int index)
{
    if (
        index < 0 ||
        index >= static_cast<int>(profiles.size())
    )
        return false;

    if (index == activeProfile)
    {
        error =
            "Switch to another profile before deleting this one.";

        return false;
    }

    Profile profile = profiles[index];

    std::error_code ec;

    fs::remove_all(
        ProfilePath(profile),
        ec
    );

    if (ec)
    {
        error =
            "Could not delete profile files: " +
            ec.message();

        return false;
    }

    profiles.erase(
        profiles.begin() + index
    );

    if (selectedProfile >= static_cast<int>(profiles.size()))
        selectedProfile =
            static_cast<int>(profiles.size()) - 1;

    SaveSettings();

    status = "Deleted profile: " + profile.name;
    error.clear();

    return true;
}

// ============================================================
// MIGRATE OLD INSTALLED DATABASE
// ============================================================

void MigrateOldInstalledDatabase()
{
    if (profiles.empty())
        return;

    fs::path oldDatabase =
        fs::u8path("cyzon_installed.json");

    if (!fs::exists(oldDatabase))
        return;

    // Only migrate when the default profile is empty.

    Profile& profile = profiles[0];

    if (!profile.mods.empty())
        return;

    std::ifstream file(oldDatabase, std::ios::binary);

    if (!file)
        return;

    try
    {
        json data = json::parse(file);

        if (
            !data.contains("mods") ||
            !data["mods"].is_array()
        )
            return;

        std::error_code ec;

        fs::create_directories(
            ProfileModsPath(profile),
            ec
        );

        fs::create_directories(
            ProfileDisabledPath(profile),
            ec
        );

        for (const auto& item : data["mods"])
        {
            InstalledMod mod;

            mod.name = item.value("name", std::string{});
            mod.description = item.value("description", std::string{});
            mod.version = item.value("version", std::string{});
            mod.file = item.value("file", std::string{});

            mod.repoURL = item.value("repo_url", std::string{});
            mod.repoLabel = item.value("repo_label", std::string{});

            mod.enabled = item.value("enabled", true);

            if (
                !IsValidDLL(mod.file) ||
                mod.repoURL.empty()
            )
                continue;

            fs::path source =
                (
                    mod.enabled
                    ? fs::u8path(modsFolder)
                    : fs::u8path(modsFolder + ".cyzon-disabled")
                ) / fs::u8path(mod.file);

            fs::path destination =
                ProfileModPath(profile, mod);

            if (fs::exists(source, ec))
            {
                fs::copy_file(
                    source,
                    destination,
                    fs::copy_options::overwrite_existing,
                    ec
                );

                ec.clear();
            }

            profile.mods.push_back(mod);
        }

        SaveProfile(profile);

        status =
            "Migrated your existing installed mods "
            "into the Default profile.";

        // Keep the old database as a backup.
        fs::rename(
            oldDatabase,
            "cyzon_installed.migrated.json",
            ec
        );
    }
    catch (...)
    {
        error =
            "Could not migrate the old installed-mod database.";
    }
}

// ============================================================
// VERSION COMPARISON
// ============================================================

int CompareVersions(
    const std::string& a,
    const std::string& b
)
{
    size_t ia = 0;
    size_t ib = 0;

    while (ia < a.size() || ib < b.size())
    {
        unsigned long long va = 0;
        unsigned long long vb = 0;

        while (
            ia < a.size() &&
            !std::isdigit(static_cast<unsigned char>(a[ia]))
        )
            ++ia;

        while (
            ib < b.size() &&
            !std::isdigit(static_cast<unsigned char>(b[ib]))
        )
            ++ib;

        while (
            ia < a.size() &&
            std::isdigit(static_cast<unsigned char>(a[ia]))
        )
        {
            unsigned digit =
                static_cast<unsigned>(a[ia] - '0');

            if (va < 1000000000000000000ULL)
                va = va * 10 + digit;

            ++ia;
        }

        while (
            ib < b.size() &&
            std::isdigit(static_cast<unsigned char>(b[ib]))
        )
        {
            unsigned digit =
                static_cast<unsigned>(b[ib] - '0');

            if (vb < 1000000000000000000ULL)
                vb = vb * 10 + digit;

            ++ib;
        }

        if (va < vb) return -1;
        if (va > vb) return 1;

        if (ia >= a.size() && ib >= b.size())
            break;
    }

    return 0;
}

const Mod* FindLatestMod(const InstalledMod& installed)
{
    for (const auto& repo : repositories)
    {
        if (
            repo.url != installed.repoURL ||
            !repo.loaded
        )
            continue;

        for (const auto& mod : repo.mods)
        {
            if (mod.file == installed.file)
                return &mod;
        }
    }

    return nullptr;
}

// ============================================================
// INSTALL / UPDATE MOD
// ============================================================

bool InstallMod(
    const Repository& repo,
    const Mod& mod,
    bool isUpdate = false
)
{
    error.clear();

    Profile* profile = CurrentProfile();

    if (!profile)
    {
        error = "Create or select a profile first.";
        return false;
    }

    if (modsFolder.empty())
    {
        error = "Select your game mods folder first.";
        return false;
    }

    if (!IsValidDLL(mod.file))
    {
        error = "Invalid DLL filename.";
        return false;
    }

    InstalledMod* existing =
        FindInstalled(mod.file);

    if (
        existing &&
        existing->repoURL != repo.url
    )
    {
        error =
            "This DLL is already managed by another repository.";

        return false;
    }

    // Do not overwrite DLLs belonging to another profile.
    // The profile's own files are stored separately.

    bool enabled =
        existing ? existing->enabled : true;

    fs::path gameDLL =
        fs::u8path(modsFolder) / fs::u8path(mod.file);

    if (!existing && enabled && fs::exists(gameDLL))
    {
        error =
            "An unmanaged DLL with this filename already "
            "exists in the game mods folder.";

        return false;
    }

    status =
        std::string(isUpdate ? "Updating " : "Downloading ") +
        mod.name + "...";

    std::string dllData;
    std::string requestError;

    if (!HTTPGet(
        ToWide(MakeRawURL(repo, "mods/" + mod.file)),
        dllData,
        requestError
    ))
    {
        error = "Download failed: " + requestError;
        return false;
    }

    if (dllData.empty())
    {
        error = "Downloaded DLL is empty.";
        return false;
    }

    InstalledMod record;

    record.name = mod.name;
    record.description = mod.description;
    record.version = mod.version;
    record.file = mod.file;

    record.repoURL = repo.url;
    record.repoLabel = RepositoryLabel(repo);
    record.enabled = enabled;
    record.manuallyInstalled = existing
        ? existing->manuallyInstalled
        : true;
    if (existing)
        record.packOwners = existing->packOwners;

    fs::path profileDestination =
        ProfileModPath(*profile, record);

    std::error_code ec;

    fs::create_directories(
        profileDestination.parent_path(),
        ec
    );

    if (ec)
    {
        error =
            "Could not create profile directory: " +
            ec.message();

        return false;
    }

    fs::path temporary = profileDestination;
    temporary += ".cyzon-temp";

    {
        std::ofstream file(
            temporary,
            std::ios::binary
        );

        if (!file)
        {
            error = "Could not write DLL to profile storage.";
            return false;
        }

        file.write(
            dllData.data(),
            static_cast<std::streamsize>(dllData.size())
        );

        if (!file.good())
        {
            error = "Failed writing DLL.";

            file.close();
            fs::remove(temporary, ec);

            return false;
        }
    }

    // Update the active game copy only when this profile
    // is currently active and the mod is enabled.

    if (profile == CurrentProfile() && enabled)
    {
        if (!CopyFileSafely(
            temporary,
            gameDLL,
            requestError
        ))
        {
            fs::remove(temporary, ec);

            error = requestError;
            return false;
        }
    }

    fs::remove(profileDestination, ec);
    ec.clear();

    fs::rename(
        temporary,
        profileDestination,
        ec
    );

    if (ec)
    {
        error =
            "Could not store DLL in profile: " +
            ec.message();

        return false;
    }

    if (existing)
        *existing = record;
    else
        profile->mods.push_back(record);

    SaveProfile(*profile);

    status =
        std::string(isUpdate ? "Updated " : "Installed ") +
        mod.name +
        " in profile " +
        profile->name + ".";

    return true;
}

// ============================================================
// ENABLE / DISABLE
// ============================================================

bool SetModEnabled(
    const std::string& filename,
    bool enabled
)
{
    error.clear();

    Profile* profile = CurrentProfile();

    if (!profile)
    {
        error = "No active profile.";
        return false;
    }

    InstalledMod* mod = FindInstalled(filename);

    if (!mod)
    {
        error = "Mod not found in active profile.";
        return false;
    }

    if (mod->enabled == enabled)
        return true;

    fs::path source = ProfileModPath(*profile, *mod);

    InstalledMod changed = *mod;
    changed.enabled = enabled;

    fs::path destination =
        ProfileModPath(*profile, changed);

    std::error_code ec;

    if (!fs::exists(source, ec))
    {
        error =
            "Could not find profile DLL: " +
            source.string();

        return false;
    }

    fs::create_directories(
        destination.parent_path(),
        ec
    );

    if (ec)
    {
        error = "Could not create profile directory.";
        return false;
    }

    // If this is the active profile, update the game folder too.

    fs::path gameDLL =
        fs::u8path(modsFolder) / fs::u8path(filename);

    if (enabled)
    {
        if (profile == CurrentProfile() && fs::exists(gameDLL))
        {
            error =
                "A DLL with this name already exists in the "
                "game folder. It was not overwritten.";

            return false;
        }

        if (profile == CurrentProfile())
        {
            std::string copyError;

            if (!CopyFileSafely(
                source,
                gameDLL,
                copyError
            ))
            {
                error = copyError;
                return false;
            }
        }
    }
    else
    {
        if (profile == CurrentProfile())
        {
            if (fs::exists(gameDLL, ec))
            {
                if (!fs::remove(gameDLL, ec) || ec)
                {
                    error =
                        "Could not remove active DLL. "
                        "Close Vampire Survivors first.";

                    return false;
                }
            }
        }
    }

    fs::remove(destination, ec);
    ec.clear();

    fs::rename(source, destination, ec);

    if (ec)
    {
        error =
            "Could not move DLL inside profile: " +
            ec.message();

        return false;
    }

    mod->enabled = enabled;

    SaveProfile(*profile);

    status =
        mod->name +
        (enabled ? " enabled." : " disabled.");

    return true;
}

// ============================================================
// UNINSTALL
// ============================================================

bool UninstallMod(const std::string& filename)
{
    error.clear();

    Profile* profile = CurrentProfile();

    if (!profile)
    {
        error = "No active profile.";
        return false;
    }

    auto it = std::find_if(
        profile->mods.begin(),
        profile->mods.end(),
        [&](const InstalledMod& mod)
        {
            return mod.file == filename;
        }
    );

    if (it == profile->mods.end())
    {
        error = "Mod not found.";
        return false;
    }

    InstalledMod mod = *it;

    std::error_code ec;

    fs::remove(
        ProfileModPath(*profile, mod),
        ec
    );

    if (ec)
    {
        error =
            "Could not remove profile DLL: " +
            ec.message();

        return false;
    }

    if (profile == CurrentProfile() && mod.enabled)
    {
        fs::path gameDLL =
            fs::u8path(modsFolder) / fs::u8path(mod.file);

        ec.clear();

        if (fs::exists(gameDLL, ec))
        {
            if (!fs::remove(gameDLL, ec) || ec)
            {
                error =
                    "Could not remove active game DLL. "
                    "Close Vampire Survivors first.";

                return false;
            }
        }
    }

    profile->mods.erase(it);

    SaveProfile(*profile);

    status =
        "Uninstalled " +
        mod.name +
        " from " +
        profile->name + ".";

    return true;
}

// ============================================================
// MODPACK MANAGEMENT
// ============================================================

std::string PackOwnerKey(const Repository& repo, const Modpack& pack)
{
    return repo.url + "|" + pack.id;
}

const InstalledPack* FindInstalledPack(
    const Profile& profile,
    const Repository& repo,
    const Modpack& pack
)
{
    for (const auto& installed : profile.packs)
    {
        if (
            installed.repoURL == repo.url &&
            installed.id == pack.id
        )
            return &installed;
    }

    return nullptr;
}

bool InstallModpack(const Repository& repo, const Modpack& pack)
{
    error.clear();

    Profile* profile = CurrentProfile();
    if (!profile)
    {
        error = "Create or select a profile first.";
        return false;
    }

    if (modsFolder.empty())
    {
        error = "Select your game mods folder first.";
        return false;
    }

    if (pack.mods.empty())
    {
        error = "This modpack does not contain any valid mods.";
        return false;
    }

    const std::string ownerKey = PackOwnerKey(repo, pack);

    std::vector<std::string> previousFiles;
    if (const InstalledPack* previous = FindInstalledPack(*profile, repo, pack))
        previousFiles = previous->files;

    // First validate all filename conflicts before downloading anything.
    for (const auto& mod : pack.mods)
    {
        const InstalledMod* installed = FindInstalledConst(mod.file);
        if (installed && installed->repoURL != repo.url)
        {
            error = "Modpack conflict: " + mod.file +
                " is already managed by another repository.";
            return false;
        }
    }

    status = "Installing modpack " + pack.name + "...";

    for (const auto& mod : pack.mods)
    {
        InstalledMod* installed = FindInstalled(mod.file);
        const bool existedBefore = installed != nullptr;

        bool storedDLLExists = false;
        if (installed)
        {
            std::error_code fileCheckError;
            storedDLLExists = fs::exists(
                ProfileModPath(*profile, *installed),
                fileCheckError
            );
        }

        if (
            !installed ||
            installed->version != mod.version ||
            !storedDLLExists
        )
        {
            if (!InstallMod(repo, mod, installed != nullptr))
            {
                error = "Could not install modpack member " +
                    mod.file + ": " + error;
                return false;
            }
        }

        installed = FindInstalled(mod.file);
        if (!installed)
        {
            error = "Installed mod could not be found: " + mod.file;
            return false;
        }

        if (!existedBefore)
            installed->manuallyInstalled = false;

        if (std::find(
            installed->packOwners.begin(),
            installed->packOwners.end(),
            ownerKey
        ) == installed->packOwners.end())
        {
            installed->packOwners.push_back(ownerKey);
        }

        // If it was already manually installed, keep that ownership.
        // Otherwise this pack is responsible for its lifetime.
        SaveProfile(*profile);
    }

    InstalledPack record;
    record.id = pack.id;
    record.name = pack.name;
    record.description = pack.description;
    record.version = pack.version;
    record.repoURL = repo.url;
    record.repoLabel = RepositoryLabel(repo);

    for (const auto& mod : pack.mods)
        record.files.push_back(mod.file);

    auto existingPack = std::find_if(
        profile->packs.begin(),
        profile->packs.end(),
        [&](const InstalledPack& installed)
        {
            return installed.id == pack.id &&
                   installed.repoURL == repo.url;
        }
    );

    if (existingPack != profile->packs.end())
        *existingPack = record;
    else
        profile->packs.push_back(record);

    std::vector<std::string> obsoleteFiles;
    for (const auto& oldFile : previousFiles)
    {
        const bool stillInPack = std::find(
            record.files.begin(),
            record.files.end(),
            oldFile
        ) != record.files.end();

        if (stillInPack)
            continue;

        InstalledMod* oldMod = FindInstalled(oldFile);
        if (!oldMod)
            continue;

        auto owner = std::remove(
            oldMod->packOwners.begin(),
            oldMod->packOwners.end(),
            ownerKey
        );
        oldMod->packOwners.erase(owner, oldMod->packOwners.end());

        if (oldMod->packOwners.empty() && !oldMod->manuallyInstalled)
            obsoleteFiles.push_back(oldFile);
    }

    SaveProfile(*profile);

    for (const auto& oldFile : obsoleteFiles)
    {
        if (!UninstallMod(oldFile))
            return false;
    }

    SaveProfile(*profile);

    status = "Installed/updated modpack " + pack.name +
        " in profile " + profile->name + ".";
    error.clear();
    return true;
}

bool RemoveModpack(const std::string& repoURL, const std::string& packID)
{
    error.clear();

    Profile* profile = CurrentProfile();
    if (!profile)
    {
        error = "No active profile.";
        return false;
    }

    auto packIt = std::find_if(
        profile->packs.begin(),
        profile->packs.end(),
        [&](const InstalledPack& pack)
        {
            return pack.repoURL == repoURL && pack.id == packID;
        }
    );

    if (packIt == profile->packs.end())
    {
        error = "This modpack is not installed in the active profile.";
        return false;
    }

    InstalledPack pack = *packIt;
    const std::string ownerKey = repoURL + "|" + packID;

    // Remove the pack's ownership first. Mods shared by another pack,
    // or installed manually, stay installed.
    std::vector<std::string> removeFiles;

    for (auto& mod : profile->mods)
    {
        auto owner = std::remove(
            mod.packOwners.begin(),
            mod.packOwners.end(),
            ownerKey
        );
        mod.packOwners.erase(owner, mod.packOwners.end());

        if (mod.packOwners.empty() && !mod.manuallyInstalled)
            removeFiles.push_back(mod.file);
    }

    profile->packs.erase(packIt);
    SaveProfile(*profile);

    for (const auto& filename : removeFiles)
    {
        if (!UninstallMod(filename))
            return false;
    }

    SaveProfile(*profile);
    status = "Removed modpack " + pack.name +
        ". Mods used elsewhere were kept.";
    error.clear();
    return true;
}

void DrawModpack(const Repository& repo, const Modpack& pack)
{
    ImGui::PushID((repo.url + "|" + pack.id).c_str());
    ImGui::Separator();

    ImGui::Text("%s", pack.name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("v%s", pack.version.c_str());

    ImGui::TextWrapped("%s", pack.description.c_str());
    ImGui::TextDisabled(
        "%zu mods | Pack ID: %s",
        pack.mods.size(),
        pack.id.c_str()
    );

    Profile* profile = CurrentProfile();
    const InstalledPack* installed =
        profile ? FindInstalledPack(*profile, repo, pack) : nullptr;

    if (installed)
    {
        ImGui::SameLine();
        ImGui::TextColored(
            ImVec4(0.3f, 0.85f, 0.4f, 1.0f),
            "[INSTALLED]"
        );

        if (installed->version != pack.version)
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.72f, 0.2f, 1.0f),
                "Update available: %s -> %s",
                installed->version.c_str(),
                pack.version.c_str()
            );
        }

        if (ImGui::Button("Update Pack"))
            InstallModpack(repo, pack);

        ImGui::SameLine();

        if (ImGui::Button("Remove Pack"))
            RemoveModpack(repo.url, pack.id);
    }
    else
    {
        if (ImGui::Button("Install Pack"))
            InstallModpack(repo, pack);
    }

    ImGui::TextDisabled("Includes:");
    for (const auto& mod : pack.mods)
    {
        ImGui::BulletText("%s (v%s)", mod.name.c_str(), mod.version.c_str());
    }

    ImGui::PopID();
}

// ============================================================
// MOD UI
// ============================================================

void DrawAvailableMod(
    const Repository& repo,
    const Mod& mod
)
{
    ImGui::PushID(
        (repo.url + mod.file).c_str()
    );

    ImGui::Separator();

    ImGui::Text("%s", mod.name.c_str());

    ImGui::SameLine();

    ImGui::TextDisabled("v%s", mod.version.c_str());

    const InstalledMod* installed =
        FindInstalledConst(mod.file);

    bool managed =
        installed && installed->repoURL == repo.url;

    if (managed)
    {
        ImGui::SameLine();

        ImGui::TextColored(
            ImVec4(0.3f, 0.85f, 0.4f, 1.0f),
            installed->enabled
                ? "[INSTALLED]"
                : "[DISABLED]"
        );
    }
    else if (installed)
    {
        ImGui::SameLine();

        ImGui::TextColored(
            ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
            "[OTHER REPOSITORY]"
        );
    }

    ImGui::TextWrapped("%s", mod.description.c_str());

    ImGui::TextDisabled("File: %s", mod.file.c_str());

    if (managed)
    {
        if (CompareVersions(
            mod.version,
            installed->version
        ) > 0)
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.72f, 0.2f, 1.0f),
                "Update available: %s -> %s",
                installed->version.c_str(),
                mod.version.c_str()
            );

            if (ImGui::Button("Update"))
                InstallMod(repo, mod, true);
        }

        ImGui::SameLine();

        if (ImGui::Button(
            installed->enabled ? "Disable" : "Enable"
        ))
        {
            SetModEnabled(
                mod.file,
                !installed->enabled
            );
        }
    }
    else if (!installed)
    {
        if (ImGui::Button("Install"))
            InstallMod(repo, mod);
    }

    ImGui::PopID();
}

void DrawInstalledMod(const InstalledMod& mod)
{
    ImGui::PushID(mod.file.c_str());

    ImGui::Separator();

    ImGui::Text("%s", mod.name.c_str());

    ImGui::SameLine();

    ImGui::TextDisabled("v%s", mod.version.c_str());

    ImGui::SameLine();

    ImGui::TextColored(
        mod.enabled
            ? ImVec4(0.3f, 0.85f, 0.4f, 1.0f)
            : ImVec4(1.0f, 0.72f, 0.2f, 1.0f),
        mod.enabled ? "[ENABLED]" : "[DISABLED]"
    );

    ImGui::TextWrapped("%s", mod.description.c_str());

    ImGui::TextDisabled(
        "%s | Source: %s",
        mod.file.c_str(),
        mod.repoLabel.c_str()
    );

    const Mod* latest = FindLatestMod(mod);

    if (latest)
    {
        if (CompareVersions(
            latest->version,
            mod.version
        ) > 0)
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.72f, 0.2f, 1.0f),
                "Update available: %s",
                latest->version.c_str()
            );

            if (ImGui::Button("Update"))
            {
                for (const auto& repo : repositories)
                {
                    if (
                        repo.url == mod.repoURL &&
                        repo.loaded
                    )
                    {
                        InstallMod(repo, *latest, true);
                        break;
                    }
                }
            }

            ImGui::SameLine();
        }
    }
    else
    {
        ImGui::TextDisabled(
            "Reload the source repository to check for updates."
        );
    }

    if (ImGui::Button(
        mod.enabled ? "Disable" : "Enable"
    ))
    {
        SetModEnabled(
            mod.file,
            !mod.enabled
        );
    }

    ImGui::SameLine();

    if (ImGui::Button("Uninstall"))
        UninstallMod(mod.file);

    ImGui::PopID();
}

// ============================================================
// FOLDER PICKER
// ============================================================

bool BrowseForFolder()
{
    BROWSEINFOW info{};

    info.lpszTitle =
        L"Select your Vampire Survivors mods folder";

    info.ulFlags =
        BIF_RETURNONLYFSDIRS |
        BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE id =
        SHBrowseForFolderW(&info);

    if (!id)
        return false;

    wchar_t path[MAX_PATH]{};

    bool success =
        SHGetPathFromIDListW(id, path);

    CoTaskMemFree(id);

    if (!success)
        return false;

    modsFolder = ToUTF8(path);

    SaveSettings();

    status = "Mods folder updated.";
    error.clear();

    return true;
}

// ============================================================
// STARTUP REPOSITORY LOADING
// ============================================================

void LoadAllSavedRepositories()
{
    for (auto& repo : repositories)
    {
        if (!repo.loaded)
        {
            if (!LoadRepository(repo))
            {
                error =
                    RepositoryLabel(repo) +
                    ": " +
                    repo.loadError;
            }
        }
    }

    if (!repositories.empty())
    {
        status =
            "Loaded " +
            std::to_string(repositories.size()) +
            " repositories.";
    }
}

// ============================================================
// MAIN
// ============================================================

int main(int, char**)
{
    if (!SDL_Init(SDL_INIT_VIDEO))
        return 1;

    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_MAJOR_VERSION, 3
    );

    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_MINOR_VERSION, 0
    );

    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_PROFILE_MASK,
        SDL_GL_CONTEXT_PROFILE_COMPATIBILITY
    );

    SDL_GL_SetAttribute(
        SDL_GL_DOUBLEBUFFER, 1
    );

    SDL_Window* window = SDL_CreateWindow(
        "CyzonVSModManager",
        1050,
        780,
        SDL_WINDOW_OPENGL |
        SDL_WINDOW_RESIZABLE
    );

    if (!window)
    {
        SDL_Quit();
        return 1;
    }

    SDL_GLContext context =
        SDL_GL_CreateContext(window);

    if (!context)
    {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_GL_MakeCurrent(window, context);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();

    io.ConfigFlags |=
        ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding = 8.0f;
    style.FrameRounding = 5.0f;
    style.GrabRounding = 5.0f;

    ImGui_ImplSDL3_InitForOpenGL(window, context);
    ImGui_ImplOpenGL3_Init("#version 130");

    CoInitializeEx(
        nullptr,
        COINIT_APARTMENTTHREADED
    );

    // Load existing settings and profiles.

    LoadSettings();

    // Create a default profile if this is a new installation.

    if (profiles.empty())
    {
        Profile profile;

        profile.id = "profile_1";
        profile.name = "Default";

        profiles.push_back(profile);

        activeProfile = 0;
        selectedProfile = 0;

        SaveProfile(profiles[0]);
        SaveSettings();

        // Import old installed mods, if available.
        MigrateOldInstalledDatabase();
    }

    if (activeProfile < 0)
    {
        activeProfile = 0;
        selectedProfile = 0;
    }

    bool running = true;
    bool initialLoad = true;

    while (running)
    {
        SDL_Event event;

        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);

            if (
                event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED
            )
            {
                running = false;
            }
        }

        if (initialLoad)
        {
            LoadAllSavedRepositories();
            initialLoad = false;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowSize(
            ImVec2(960, 740),
            ImGuiCond_FirstUseEver
        );

        ImGui::Begin("CyzonVSModManager");

        ImGui::Text("CYZON");
        ImGui::SameLine();

        ImGui::TextDisabled(
            "Vampire Survivors Mod Manager"
        );

        ImGui::Separator();

        // ====================================================
        // PROFILES
        // ====================================================

        ImGui::Text("Mod Profiles");

        ImGui::SameLine();

        ImGui::TextDisabled(
            "Switch between separate mod configurations."
        );

        if (!profiles.empty())
        {
            std::string preview =
                selectedProfile >= 0 &&
                selectedProfile < static_cast<int>(profiles.size())
                ? profiles[selectedProfile].name
                : "Select profile";

            if (ImGui::BeginCombo(
                "##profile_select",
                preview.c_str()
            ))
            {
                for (
                    int i = 0;
                    i < static_cast<int>(profiles.size());
                    ++i
                )
                {
                    bool selected = i == selectedProfile;

                    std::string label =
                        profiles[i].name;

                    if (i == activeProfile)
                        label += " (ACTIVE)";

                    if (ImGui::Selectable(
                        label.c_str(),
                        selected
                    ))
                    {
                        selectedProfile = i;
                    }

                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }

                ImGui::EndCombo();
            }

            ImGui::SameLine();

            bool canSwitch =
                selectedProfile >= 0 &&
                selectedProfile < static_cast<int>(profiles.size()) &&
                selectedProfile != activeProfile;

            if (!canSwitch)
                ImGui::BeginDisabled();

            if (ImGui::Button("Switch Profile"))
                SwitchProfile(selectedProfile);

            if (!canSwitch)
                ImGui::EndDisabled();

            ImGui::SameLine();

            bool canDelete =
                selectedProfile >= 0 &&
                selectedProfile < static_cast<int>(profiles.size()) &&
                selectedProfile != activeProfile &&
                profiles.size() > 1;

            if (!canDelete)
                ImGui::BeginDisabled();

            if (ImGui::Button("Delete Profile"))
                DeleteProfile(selectedProfile);

            if (!canDelete)
                ImGui::EndDisabled();
        }

        ImGui::SetNextItemWidth(250);

        ImGui::InputText(
            "##new_profile_name",
            profileNameInput,
            sizeof(profileNameInput)
        );

        ImGui::SameLine();

        if (ImGui::Button("Create Profile"))
        {
            if (CreateProfile(profileNameInput))
                profileNameInput[0] = '\0';
        }

        if (const Profile* profile = CurrentProfileConst())
        {
            ImGui::TextColored(
                ImVec4(0.3f, 0.85f, 0.4f, 1.0f),
                "Active profile: %s",
                profile->name.c_str()
            );

            ImGui::SameLine();

            ImGui::TextDisabled(
                "(%zu mods, %zu packs)",
                profile->mods.size(),
                profile->packs.size()
            );
        }

        ImGui::Separator();

        // ====================================================
        // REPOSITORIES
        // ====================================================

        ImGui::Text("Repositories");

        if (!repositories.empty())
        {
            std::string preview =
                selectedRepository >= 0 &&
                selectedRepository < static_cast<int>(repositories.size())
                ? RepositoryLabel(repositories[selectedRepository])
                : "Select repository";

            if (ImGui::BeginCombo(
                "##repository_select",
                preview.c_str()
            ))
            {
                for (
                    int i = 0;
                    i < static_cast<int>(repositories.size());
                    ++i
                )
                {
                    bool selected =
                        i == selectedRepository;

                    std::string label =
                        RepositoryLabel(repositories[i]);

                    if (ImGui::Selectable(
                        label.c_str(),
                        selected
                    ))
                    {
                        selectedRepository = i;

                        std::snprintf(
                            repoInput,
                            sizeof(repoInput),
                            "%s",
                            repositories[i].url.c_str()
                        );
                    }

                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }

                ImGui::EndCombo();
            }

            ImGui::SameLine();

            if (ImGui::Button("Reload"))
            {
                if (selectedRepository >= 0)
                {
                    if (!LoadRepository(
                        repositories[selectedRepository]
                    ))
                    {
                        error =
                            repositories[selectedRepository].loadError;
                    }
                    else
                    {
                        error.clear();
                    }
                }
            }

            ImGui::SameLine();

            if (ImGui::Button("Remove"))
            {
                if (
                    selectedRepository >= 0 &&
                    selectedRepository < static_cast<int>(repositories.size())
                )
                {
                    repositories.erase(
                        repositories.begin() + selectedRepository
                    );

                    selectedRepository =
                        repositories.empty() ? -1 : 0;

                    SaveSettings();
                }
            }
        }

        ImGui::SetNextItemWidth(-100);

        ImGui::InputText(
            "##repo_url",
            repoInput,
            sizeof(repoInput)
        );

        ImGui::SameLine();

        if (ImGui::Button("Add Repo"))
        {
            if (AddRepository(repoInput))
                SaveSettings();
        }

        ImGui::Separator();

        // ====================================================
        // GAME MODS FOLDER
        // ====================================================

        ImGui::Text("Vampire Survivors Mods Folder");

        ImGui::TextWrapped(
            "%s",
            modsFolder.empty()
                ? "No folder selected."
                : modsFolder.c_str()
        );

        if (ImGui::Button("Browse Folder"))
            BrowseForFolder();

        ImGui::SameLine();

        if (ImGui::Button("Save Settings"))
        {
            SaveSettings();
            SaveAllProfiles();

            status = "Settings and profiles saved.";
            error.clear();
        }

        ImGui::Separator();

        // ====================================================
        // MOD TABS
        // ====================================================

        if (ImGui::BeginTabBar("MainTabs"))
        {
            // ----------------------------------------------
            // AVAILABLE MODS
            // ----------------------------------------------

            if (ImGui::BeginTabItem("Available Mods"))
            {
                ImGui::BeginChild(
                    "AvailableModsList",
                    ImVec2(0, -45),
                    true
                );

                if (
                    selectedRepository < 0 ||
                    selectedRepository >= static_cast<int>(repositories.size())
                )
                {
                    ImGui::TextDisabled(
                        "Add or select a repository."
                    );
                }
                else
                {
                    const Repository& repo =
                        repositories[selectedRepository];

                    if (!repo.loaded)
                    {
                        ImGui::TextDisabled(
                            "Repository has not loaded."
                        );
                    }
                    else
                    {
                        for (const Mod& mod : repo.mods)
                            DrawAvailableMod(repo, mod);
                    }
                }

                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            // ----------------------------------------------
            // MODPACKS
            // ----------------------------------------------

            if (ImGui::BeginTabItem("Modpacks"))
            {
                ImGui::BeginChild(
                    "ModpacksList",
                    ImVec2(0, -45),
                    true
                );

                if (
                    selectedRepository < 0 ||
                    selectedRepository >= static_cast<int>(repositories.size())
                )
                {
                    ImGui::TextDisabled("Add or select a repository.");
                }
                else
                {
                    const Repository& repo = repositories[selectedRepository];

                    if (!repo.loaded)
                    {
                        ImGui::TextDisabled("Repository has not loaded.");
                    }
                    else if (repo.modpacks.empty())
                    {
                        ImGui::TextDisabled(
                            "This repository has no valid modpacks.json entries."
                        );
                    }
                    else
                    {
                        for (const Modpack& pack : repo.modpacks)
                            DrawModpack(repo, pack);
                    }
                }

                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            // ----------------------------------------------
            // INSTALLED MODS
            // ----------------------------------------------

            if (ImGui::BeginTabItem("Installed Mods"))
            {
                ImGui::BeginChild(
                    "InstalledModsList",
                    ImVec2(0, -45),
                    true
                );

                Profile* profile = CurrentProfile();

                if (!profile || profile->mods.empty())
                {
                    ImGui::TextDisabled(
                        "No mods installed in this profile."
                    );
                }
                else
                {
                    for (size_t i = 0; i < profile->mods.size(); ++i)
                    {
                        size_t previousSize =
                            profile->mods.size();

                        DrawInstalledMod(profile->mods[i]);

                        if (profile->mods.size() != previousSize)
                            break;
                    }
                }

                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        // ====================================================
        // STATUS
        // ====================================================

        ImGui::Separator();

        if (!error.empty())
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                "%s",
                error.c_str()
            );
        }
        else
        {
            ImGui::TextWrapped("%s", status.c_str());
        }

        ImGui::End();

        // ====================================================
        // RENDER
        // ====================================================

        ImGui::Render();

        int width = 0;
        int height = 0;

        SDL_GetWindowSizeInPixels(
            window,
            &width,
            &height
        );

        glViewport(0, 0, width, height);

        glClearColor(
            0.075f,
            0.085f,
            0.11f,
            1.0f
        );

        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(
            ImGui::GetDrawData()
        );

        SDL_GL_SwapWindow(window);
    }

    // ========================================================
    // SHUTDOWN
    // ========================================================

    SaveSettings();
    SaveAllProfiles();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();

    ImGui::DestroyContext();

    SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(window);

    SDL_Quit();
    CoUninitialize();

    return 0;
}
