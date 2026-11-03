
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <commdlg.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include "external/imgui/imgui.h"
#include "external/imgui/backends/imgui_impl_sdl3.h"
#include "external/imgui/backends/imgui_impl_opengl3.h"
#include "external/json/json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
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
static char modTemplateName[128] = "MyMelonMod";
static char modTemplateAuthor[128] = "Author Name";
static int modTemplateFramework = 0;

static std::string modsFolder;
static std::string gameFolder;

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
    file << "game_folder=" << gameFolder << '\n';

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
        else if (key == "game_folder")
        {
            gameFolder = value;
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

// ============================================================
// CVSM ARCHIVES + MELONLOADER FIX INSTALLER
// .cvsm is a ZIP archive containing manifest.json and mods/*.dll.
// PowerShell's built-in ZIP support avoids an extra dependency.
// ============================================================

std::wstring PSQuote(const fs::path& path)
{
    std::wstring value = path.wstring();
    std::wstring quoted = L"'";
    for (wchar_t ch : value)
    {
        if (ch == L'\'') quoted += L"''";
        else quoted += ch;
    }
    quoted += L"'";
    return quoted;
}

std::string Base64Encode(const unsigned char* data, size_t size)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (size_t i = 0; i < size; i += 3)
    {
        unsigned a = data[i];
        unsigned b = i + 1 < size ? data[i + 1] : 0;
        unsigned c = i + 2 < size ? data[i + 2] : 0;
        unsigned n = (a << 16) | (b << 8) | c;
        out.push_back(table[(n >> 18) & 63]);
        out.push_back(table[(n >> 12) & 63]);
        out.push_back(i + 1 < size ? table[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < size ? table[n & 63] : '=');
    }
    return out;
}

bool RunPowerShell(const std::wstring& script, std::string& message)
{
    // PowerShell -EncodedCommand requires UTF-16LE bytes.
    std::vector<unsigned char> utf16(script.size() * sizeof(wchar_t));
    if (!utf16.empty())
        memcpy(utf16.data(), script.data(), utf16.size());
    std::string encoded = Base64Encode(utf16.data(), utf16.size());

    std::wstring command =
        L"powershell.exe -NoLogo -NoProfile -NonInteractive "
        L"-ExecutionPolicy Bypass -EncodedCommand " + ToWide(encoded);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};

    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    if (!CreateProcessW(
        nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process
    ))
    {
        message = "Could not start PowerShell. ZIP support requires Windows PowerShell.";
        return false;
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    if (exitCode != 0)
    {
        message = "PowerShell could not process the ZIP archive (exit code " +
            std::to_string(exitCode) + ").";
        return false;
    }
    return true;
}

fs::path MakeTemporaryDirectory(const std::string& prefix)
{
    fs::path path = fs::temp_directory_path() /
        fs::u8path(prefix + "_" + std::to_string(GetTickCount64()));
    std::error_code ec;
    fs::create_directories(path, ec);
    return ec ? fs::path{} : path;
}

bool ChooseCVSMSavePath(fs::path& path)
{
    wchar_t filename[MAX_PATH] = L"MyModpack.cvsm";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = filename;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = L"Cyzon Vampire Survivors Modpack (*.cvsm)\0*.cvsm\0All files (*.*)\0*.*\0\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    dialog.lpstrDefExt = L"cvsm";
    if (!GetSaveFileNameW(&dialog)) return false;
    path = filename;
    if (path.extension() != L".cvsm") path += L".cvsm";
    return true;
}

bool ChooseCVSMOpenPath(fs::path& path)
{
    wchar_t filename[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = filename;
    dialog.nMaxFile = 32768;
    dialog.lpstrFilter = L"Cyzon Vampire Survivors Modpack (*.cvsm)\0*.cvsm\0All files (*.*)\0*.*\0\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&dialog)) return false;
    path = filename;
    return true;
}

bool ExportCVSMPack(const InstalledPack& pack)
{
    const Profile* profile = CurrentProfileConst();
    if (!profile) { error = "No active profile."; return false; }

    fs::path output;
    if (!ChooseCVSMSavePath(output)) return false;

    fs::path staging = MakeTemporaryDirectory("cyzon_cvsm_export");
    if (staging.empty()) { error = "Could not create temporary export folder."; return false; }

    std::error_code ec;
    fs::create_directories(staging / "mods", ec);
    if (ec) { error = "Could not create archive staging folder."; return false; }

    json manifest;
    manifest["format"] = "CVSM";
    manifest["format_version"] = 1;
    manifest["id"] = pack.id;
    manifest["name"] = pack.name;
    manifest["description"] = pack.description;
    manifest["version"] = pack.version;
    manifest["mods"] = json::array();

    for (const auto& filename : pack.files)
    {
        const InstalledMod* installed = nullptr;
        for (const auto& mod : profile->mods)
            if (mod.file == filename) { installed = &mod; break; }

        if (!installed || !IsValidDLL(filename))
        {
            error = "Cannot export missing or invalid mod: " + filename;
            fs::remove_all(staging, ec);
            return false;
        }

        fs::path source = ProfileModPath(*profile, *installed);
        fs::path destination = staging / "mods" / fs::u8path(filename);
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            error = "Could not copy " + filename + " into the archive: " + ec.message();
            fs::remove_all(staging, ec);
            return false;
        }

        manifest["mods"].push_back({
            {"name", installed->name},
            {"description", installed->description},
            {"version", installed->version},
            {"file", installed->file}
        });
    }

    {
        std::ofstream file(staging / "manifest.json", std::ios::binary);
        if (!file) { error = "Could not write modpack manifest."; fs::remove_all(staging, ec); return false; }
        file << manifest.dump(4);
    }

    fs::path zipPath = staging;
    zipPath += L".zip";
    std::wstring script = L"$ErrorActionPreference='Stop'; Compress-Archive -Path " +
        PSQuote(staging / L"*") + L" -DestinationPath " + PSQuote(zipPath) + L" -Force";
    std::string psError;
    if (!RunPowerShell(script, psError))
    {
        error = psError;
        fs::remove_all(staging, ec);
        return false;
    }

    fs::copy_file(zipPath, output, fs::copy_options::overwrite_existing, ec);
    fs::remove(zipPath, ec);
    fs::remove_all(staging, ec);
    if (ec) { error = "Could not finish writing the .cvsm file."; return false; }

    status = "Exported " + pack.name + " as a .cvsm modpack.";
    error.clear();
    return true;
}

bool ImportCVSMPack()
{
    if (modsFolder.empty()) { error = "Select the Vampire Survivors Mods folder first."; return false; }
    Profile* profile = CurrentProfile();
    if (!profile) { error = "No active profile."; return false; }

    fs::path archive;
    if (!ChooseCVSMOpenPath(archive)) return false;

    fs::path staging = MakeTemporaryDirectory("cyzon_cvsm_import");
    if (staging.empty()) { error = "Could not create temporary import folder."; return false; }

    fs::path zipPath = staging / "import.zip";
    std::error_code ec;
    fs::copy_file(archive, zipPath, fs::copy_options::overwrite_existing, ec);
    if (ec) { error = "Could not read selected .cvsm file."; fs::remove_all(staging, ec); return false; }

    std::wstring script = L"$ErrorActionPreference='Stop'; Expand-Archive -LiteralPath " +
        PSQuote(zipPath) + L" -DestinationPath " + PSQuote(staging / L"contents") + L" -Force";
    std::string psError;
    if (!RunPowerShell(script, psError))
    {
        error = psError;
        fs::remove_all(staging, ec);
        return false;
    }

    json manifest;
    try
    {
        std::ifstream file(staging / "contents" / "manifest.json", std::ios::binary);
        if (!file) throw std::runtime_error("manifest.json is missing");
        file >> manifest;
    }
    catch (const std::exception& ex)
    {
        error = std::string("Invalid .cvsm archive: ") + ex.what();
        fs::remove_all(staging, ec);
        return false;
    }

    if (manifest.value("format", std::string{}) != "CVSM" ||
        manifest.value("format_version", 0) != 1 ||
        !manifest.contains("mods") || !manifest["mods"].is_array())
    {
        error = "Unsupported or invalid .cvsm format.";
        fs::remove_all(staging, ec);
        return false;
    }

    InstalledPack pack;
    pack.id = manifest.value("id", std::string{});
    pack.name = manifest.value("name", std::string("Imported Modpack"));
    pack.description = manifest.value("description", std::string{});
    pack.version = manifest.value("version", std::string("1.0.0"));
    pack.repoURL = "local-cvsm://" + pack.id;
    pack.repoLabel = "Imported .cvsm archive";

    if (pack.id.empty() || pack.id.find_first_of("/\\:") != std::string::npos)
    {
        error = "The .cvsm pack has an invalid ID.";
        fs::remove_all(staging, ec);
        return false;
    }

    for (const auto& oldPack : profile->packs)
    {
        if (oldPack.repoURL == pack.repoURL && oldPack.id == pack.id)
        {
            error = "A local modpack with this ID is already installed. Remove it before importing again.";
            fs::remove_all(staging, ec);
            return false;
        }
    }

    struct PendingMod { InstalledMod record; fs::path source; };
    std::vector<PendingMod> pending;
    std::vector<std::string> filenames;

    for (const auto& item : manifest["mods"])
    {
        if (!item.is_object()) continue;
        InstalledMod mod;
        mod.name = item.value("name", std::string{});
        mod.description = item.value("description", std::string{});
        mod.version = item.value("version", std::string("1.0.0"));
        mod.file = item.value("file", std::string{});
        mod.repoURL = pack.repoURL;
        mod.repoLabel = pack.repoLabel;
        mod.enabled = true;
        mod.manuallyInstalled = false;
        mod.packOwners.push_back(pack.repoURL + "|" + pack.id);

        if (!IsValidDLL(mod.file) ||
            std::find(filenames.begin(), filenames.end(), mod.file) != filenames.end())
        {
            error = "The .cvsm manifest contains an invalid or duplicate DLL filename.";
            fs::remove_all(staging, ec);
            return false;
        }

        fs::path source = staging / "contents" / "mods" / fs::u8path(mod.file);
        if (!fs::exists(source, ec) || fs::is_symlink(fs::symlink_status(source, ec)))
        {
            error = "The .cvsm archive is missing " + mod.file;
            fs::remove_all(staging, ec);
            return false;
        }

        if (FindInstalledConst(mod.file))
        {
            error = "A mod named " + mod.file + " is already installed. Remove or rename it before importing this pack.";
            fs::remove_all(staging, ec);
            return false;
        }

        fs::path gameDLL = fs::u8path(modsFolder) / fs::u8path(mod.file);
        if (fs::exists(gameDLL, ec))
        {
            error = "An unmanaged DLL already exists in the game Mods folder: " + mod.file;
            fs::remove_all(staging, ec);
            return false;
        }

        filenames.push_back(mod.file);
        pending.push_back({mod, source});
    }

    if (pending.empty())
    {
        error = "This .cvsm archive does not contain any mods.";
        fs::remove_all(staging, ec);
        return false;
    }

    fs::create_directories(ProfileModsPath(*profile), ec);
    std::vector<fs::path> copiedProfile;
    std::vector<fs::path> copiedGame;

    for (const auto& entry : pending)
    {
        fs::path profilePath = ProfileModsPath(*profile) / fs::u8path(entry.record.file);
        fs::path gamePath = fs::u8path(modsFolder) / fs::u8path(entry.record.file);
        if (!CopyFileSafely(entry.source, profilePath, error) ||
            !CopyFileSafely(profilePath, gamePath, error))
        {
            for (const auto& path : copiedProfile) fs::remove(path, ec);
            for (const auto& path : copiedGame) fs::remove(path, ec);
            fs::remove(profilePath, ec);
            fs::remove(gamePath, ec);
            fs::remove_all(staging, ec);
            return false;
        }
        copiedProfile.push_back(profilePath);
        copiedGame.push_back(gamePath);
        profile->mods.push_back(entry.record);
        pack.files.push_back(entry.record.file);
    }

    profile->packs.push_back(pack);
    SaveProfile(*profile);
    fs::remove_all(staging, ec);
    status = "Imported .cvsm modpack " + pack.name + " into profile " + profile->name + ".";
    error.clear();
    return true;
}

bool BrowseForGameFolder()
{
    BROWSEINFOW info{};
    info.lpszTitle = L"Select the Vampire Survivors game installation folder (the folder containing VampireSurvivors.exe)";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&info);
    if (!id) return false;
    wchar_t path[MAX_PATH]{};
    bool success = SHGetPathFromIDListW(id, path);
    CoTaskMemFree(id);
    if (!success) return false;
    gameFolder = ToUTF8(path);
    SaveSettings();
    status = "Vampire Survivors game folder updated.";
    error.clear();
    return true;
}

std::string EscapeXML(const std::string& value)
{
    std::string escaped;
    for (char ch : value)
    {
        switch (ch)
        {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        case '\'': escaped += "&apos;"; break;
        default: escaped += ch; break;
        }
    }
    return escaped;
}

std::string EscapeCSharpString(const std::string& value)
{
    std::string escaped;
    for (char ch : value)
    {
        switch (ch)
        {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped += ch; break;
        }
    }
    return escaped;
}

bool WriteTemplateFile(const fs::path& path, const std::string& contents)
{
    std::ofstream file(path, std::ios::binary);
    if (!file)
        return false;

    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return file.good();
}

bool ChooseTemplateParentFolder(fs::path& path)
{
    BROWSEINFOW info{};
    info.lpszTitle = L"Select where to create the MelonLoader mod template";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&info);
    if (!id)
        return false;

    wchar_t selectedPath[MAX_PATH]{};
    bool success = SHGetPathFromIDListW(id, selectedPath);
    CoTaskMemFree(id);
    if (!success)
    {
        error = "Could not read the selected template location.";
        return false;
    }

    path = selectedPath;
    return true;
}

bool CreateMelonLoaderModTemplate()
{
    if (gameFolder.empty())
    {
        error = "Select the Vampire Survivors game folder first.";
        return false;
    }

    fs::path gamePath = fs::u8path(gameFolder);
    std::error_code ec;
    if (!fs::exists(gamePath / "VampireSurvivors.exe", ec))
    {
        error = ec
            ? "Could not inspect the selected game folder: " + ec.message()
            : "The selected game folder must contain VampireSurvivors.exe.";
        return false;
    }

    fs::path melonLoaderAssembly = gamePath / "MelonLoader" / "net6" / "MelonLoader.dll";
    ec.clear();
    if (!fs::is_regular_file(melonLoaderAssembly, ec))
    {
        std::string assemblyPath = ToUTF8(melonLoaderAssembly.wstring());
        error = ec
            ? "Could not inspect " + assemblyPath + ": " + ec.message()
            : "MelonLoader.dll was not found at " + assemblyPath +
                ". Confirm the selected game folder and browse to its root folder.";
        return false;
    }

    std::string projectName = modTemplateName;
    if (projectName.empty() ||
        !(std::isalpha(static_cast<unsigned char>(projectName[0])) || projectName[0] == '_') ||
        !std::all_of(projectName.begin(), projectName.end(), [](unsigned char ch)
        {
            return std::isalnum(ch) || ch == '_';
        }))
    {
        error = "The template name must be a valid C# identifier (letters, numbers, and underscores; it cannot start with a number).";
        return false;
    }

    fs::path parent;
    if (!ChooseTemplateParentFolder(parent))
        return false;
    fs::path output = parent / fs::u8path(projectName);
    if (fs::exists(output, ec))
    {
        error = "A folder named " + projectName + " already exists in the selected location.";
        return false;
    }
    if (ec || !fs::create_directory(output, ec))
    {
        error = "Could not create the template folder" +
            (ec ? ": " + ec.message() : ".");
        return false;
    }

    std::string targetFramework;
    switch (modTemplateFramework)
    {
    case 0: targetFramework = "netstandard2.1"; break;
    case 1: targetFramework = "net472"; break;
    case 2: targetFramework = "net6.0"; break;
    default:
        error = "Select a supported template framework.";
        fs::remove(output, ec);
        return false;
    }

    std::string projectFile =
        "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
        "  <PropertyGroup>\n"
        "    <TargetFramework>" + targetFramework + "</TargetFramework>\n"
        "    <AssemblyName>" + EscapeXML(projectName) + "</AssemblyName>\n"
        "    <RootNamespace>" + EscapeXML(projectName) + "</RootNamespace>\n"
        "  </PropertyGroup>\n"
        "  <ItemGroup>\n"
        "    <Reference Include=\"MelonLoader\">\n"
        "      <HintPath>" + EscapeXML(ToUTF8(melonLoaderAssembly.wstring())) + "</HintPath>\n"
        "      <Private>false</Private>\n"
        "    </Reference>\n"
        "  </ItemGroup>\n"
        "</Project>\n";

    std::string sourceFile =
        "using MelonLoader;\n\n"
        "[assembly: MelonInfo(typeof(MyMod), \"" + EscapeCSharpString(projectName) +
        "\", \"1.0.0\", \"" + EscapeCSharpString(modTemplateAuthor) + "\")]\n"
        "[assembly: MelonGame(\"\", \"\")]\n\n"
        "public class MyMod : MelonMod\n"
        "{\n"
        "    public override void OnInitializeMelon()\n"
        "    {\n"
        "        LoggerInstance.Msg(\"Hello from " + EscapeCSharpString(projectName) + "!\");\n"
        "    }\n"
        "}\n";

    std::string readme =
        "# " + projectName + "\n\n"
        "A starter MelonLoader mod project generated by CyzonVSModManager.\n\n"
        "## Build\n\n"
        "Open `" + projectName + ".csproj` in Visual Studio or run `dotnet build` "
        "with the matching .NET targeting pack installed. The project references "
        "`MelonLoader.dll` from the selected Vampire Survivors installation.\n\n"
        "The generated `MelonGame` attribute leaves both game values empty, as "
        "allowed by the MelonLoader guide. Set them to the exact values from the "
        "game's `app.info` file if the mod should be limited to a specific game.\n\n"
        "For IL2CPP mods or mods using Unity/Harmony APIs, add the additional "
        "references required by the game and MelonLoader version.\n\n"
        "See the [official MelonLoader modding guide]"
        "(https://melonwiki.xyz/#/modders/quickstart) for framework selection, "
        "references, metadata, and callbacks.\n";

    if (!WriteTemplateFile(output / (projectName + ".csproj"), projectFile) ||
        !WriteTemplateFile(output / "MyMod.cs", sourceFile) ||
        !WriteTemplateFile(output / "README.md", readme))
    {
        error = "Could not write all template files in " + ToUTF8(output.wstring()) +
            ". Check that the location is writable.";
        return false;
    }

    status = "Created the MelonLoader mod template at " + ToUTF8(output.wstring()) + ".";
    error.clear();
    return true;
}

bool OpenMelonLoaderModdingDocs()
{
    HINSTANCE result = ShellExecuteW(
        nullptr,
        L"open",
        L"https://melonwiki.xyz/#/modders/quickstart",
        nullptr,
        nullptr,
        SW_SHOWNORMAL
    );
    if (reinterpret_cast<INT_PTR>(result) <= 32)
    {
        error = "Could not open the MelonLoader modding guide in your browser.";
        return false;
    }

    status = "Opened the official MelonLoader modding guide.";
    error.clear();
    return true;
}

bool InstallMelonLoaderFix()
{
    if (gameFolder.empty())
    {
        error = "Select the Vampire Survivors game installation folder first.";
        return false;
    }

    if (!fs::exists(fs::u8path(gameFolder) / "VampireSurvivors.exe"))
    {
        error = "The selected folder does not contain VampireSurvivors.exe. Select the game's root installation folder.";
        return false;
    }

    // This repository is expected to publish a ZIP-able main branch.
    const std::wstring url = L"https://github.com/cyzon123456792/melon-loader-vs-fix/archive/refs/heads/main.zip";
    status = "Downloading the MelonLoader Vampire Survivors fix...";
    std::string archiveData, requestError;
    if (!HTTPGet(url, archiveData, requestError))
    {
        error = "Could not download the MelonLoader fix: " + requestError +
            ". Check that the repository has files on its main branch and is public.";
        return false;
    }

    fs::path staging = MakeTemporaryDirectory("cyzon_melonloader");
    if (staging.empty()) { error = "Could not create temporary installer folder."; return false; }
    fs::path zipPath = staging / "melon-loader-fix.zip";
    {
        std::ofstream file(zipPath, std::ios::binary);
        if (!file) { error = "Could not save downloaded ZIP."; return false; }
        file.write(archiveData.data(), static_cast<std::streamsize>(archiveData.size()));
    }

    std::wstring script = L"$ErrorActionPreference='Stop'; Expand-Archive -LiteralPath " +
        PSQuote(zipPath) + L" -DestinationPath " + PSQuote(staging / L"extracted") + L" -Force";
    std::string psError;
    if (!RunPowerShell(script, psError))
    {
        error = "Could not extract MelonLoader fix: " + psError;
        std::error_code ec; fs::remove_all(staging, ec); return false;
    }

    fs::path sourceRoot = staging / "extracted";
    std::error_code ec;
    if (!fs::exists(sourceRoot, ec) || ec)
    {
        error = "The downloaded ZIP did not extract correctly.";
        fs::remove_all(staging, ec);
        return false;
    }

    // GitHub source archives normally contain one top-level folder.
    // Flatten that wrapper so its contents land in the game root.
    fs::directory_iterator rootIt(sourceRoot, ec), rootEnd;
    if (!ec)
    {
        std::vector<fs::directory_entry> rootEntries;
        for (; rootIt != rootEnd; rootIt.increment(ec))
        {
            if (ec) break;
            rootEntries.push_back(*rootIt);
        }
        if (!ec && rootEntries.size() == 1 && rootEntries[0].is_directory())
            sourceRoot = rootEntries[0].path();
    }
    ec.clear();
    size_t copied = 0;
    for (fs::recursive_directory_iterator it(sourceRoot, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec))
    {
        if (it->is_symlink(ec)) { error = "The downloaded archive contains a symbolic link; installation stopped."; fs::remove_all(staging, ec); return false; }
        if (!it->is_regular_file(ec)) continue;
        fs::path relative = fs::relative(it->path(), sourceRoot, ec);
        if (ec || relative.empty() || relative.string().find("..") != std::string::npos)
        {
            error = "The downloaded archive contains an invalid path.";
            fs::remove_all(staging, ec);
            return false;
        }
        fs::path destination = fs::u8path(gameFolder) / relative;
        if (!CopyFileSafely(it->path(), destination, error))
        {
            fs::remove_all(staging, ec);
            return false;
        }
        ++copied;
    }

    fs::remove_all(staging, ec);
    if (copied == 0)
    {
        error = "The MelonLoader fix ZIP did not contain any files.";
        return false;
    }
    status = "Installed the MelonLoader Vampire Survivors fix (" + std::to_string(copied) + " files).";
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

        if (ImGui::Button("Browse Mods Folder"))
            BrowseForFolder();

        ImGui::Separator();
        ImGui::Text("Vampire Survivors Game Installation Folder");
        ImGui::TextWrapped(
            "%s",
            gameFolder.empty()
                ? "No game installation folder selected. Needed for MelonLoader installation."
                : gameFolder.c_str()
        );
        if (ImGui::Button("Browse Game Folder"))
            BrowseForGameFolder();

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
            // SHARING + MELONLOADER
            // ----------------------------------------------

            if (ImGui::BeginTabItem("Sharing & Loader"))
            {
                ImGui::Text("CVSM Modpack Sharing");
                ImGui::TextWrapped(
                    "Export an installed repository modpack as a portable .cvsm file, or import a .cvsm file shared by someone else."
                );
                ImGui::Separator();

                Profile* sharingProfile = CurrentProfile();
                if (!sharingProfile || sharingProfile->packs.empty())
                {
                    ImGui::TextDisabled("No modpacks are installed in this profile yet.");
                }
                else
                {
                    for (const auto& installedPack : sharingProfile->packs)
                    {
                        ImGui::PushID((installedPack.repoURL + "|" + installedPack.id).c_str());
                        ImGui::Text("%s", installedPack.name.c_str());
                        ImGui::SameLine();
                        ImGui::TextDisabled("v%s | %zu DLLs", installedPack.version.c_str(), installedPack.files.size());
                        if (ImGui::Button("Export .cvsm"))
                            ExportCVSMPack(installedPack);
                        ImGui::Separator();
                        ImGui::PopID();
                    }
                }

                if (ImGui::Button("Import .cvsm Modpack"))
                    ImportCVSMPack();

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Text("Create a MelonLoader Mod Template");
                ImGui::TextWrapped(
                    "Generate a starter C# project with MelonMod, MelonInfo, MelonGame, and a reference to MelonLoader.dll. Install MelonLoader and run the game once first."
                );
                if (!gameFolder.empty())
                {
                    fs::path expectedAssembly =
                        fs::u8path(gameFolder) / "MelonLoader" / "net6" / "MelonLoader.dll";
                    ImGui::TextWrapped(
                        "Expected MelonLoader.dll: %s",
                        ToUTF8(expectedAssembly.wstring()).c_str()
                    );
                }
                ImGui::InputText(
                    "Project name",
                    modTemplateName,
                    sizeof(modTemplateName)
                );
                ImGui::InputText(
                    "Author",
                    modTemplateAuthor,
                    sizeof(modTemplateAuthor)
                );
                const char* frameworkOptions[] = {
                    "Mono - .NET Standard 2.1 (Unity 2021.2+)",
                    "Mono - .NET Framework 4.7.2 (Unity 2018.1+)",
                    "IL2CPP - .NET 6.0"
                };
                ImGui::Combo(
                    "Target framework",
                    &modTemplateFramework,
                    frameworkOptions,
                    IM_ARRAYSIZE(frameworkOptions)
                );
                if (ImGui::Button("Create Mod Template"))
                    CreateMelonLoaderModTemplate();
                ImGui::SameLine();
                if (ImGui::Button("Open MelonLoader Modding Docs"))
                    OpenMelonLoaderModdingDocs();

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Text("MelonLoader Vampire Survivors Fix");
                ImGui::TextWrapped(
                    "Downloads the ZIP from cyzon123456792/melon-loader-vs-fix and extracts its contents into the selected game installation folder. Close Vampire Survivors before installing."
                );
                ImGui::TextDisabled("Game folder: %s", gameFolder.empty() ? "Not selected" : gameFolder.c_str());
                if (gameFolder.empty()) ImGui::BeginDisabled();
                if (ImGui::Button("Download & Install MelonLoader Fix"))
                    InstallMelonLoaderFix();
                if (gameFolder.empty()) ImGui::EndDisabled();

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
