using System.Net;
using System.Net.Http.Headers;
using System.Reflection;
using System.Text.Json;

namespace MbfTwain.VirtualScannerConfig.Updates;

internal sealed class GitHubUpdateService
{
    private const string RepositoryOwner = "mingbingfeng";
    private const string RepositoryName = "mbfTWAIN";
    private static readonly Uri LatestReleaseUri = new($"https://api.github.com/repos/{RepositoryOwner}/{RepositoryName}/releases/latest");
    private static readonly HttpClient HttpClient = CreateHttpClient();

    public Version CurrentVersion { get; } = GetCurrentVersion();

    public string CurrentVersionText => CurrentVersion.ToString(3);

    public async Task<ReleaseUpdateInfo> GetLatestReleaseAsync(CancellationToken cancellationToken)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, LatestReleaseUri);
        using HttpResponseMessage response = await HttpClient.SendAsync(request, cancellationToken).ConfigureAwait(false);
        if (response.StatusCode == HttpStatusCode.NotFound)
        {
            throw new InvalidOperationException(
                "无法访问 GitHub Release。仓库为私有时，请设置 MBF_TWAIN_GITHUB_TOKEN 后再检查更新。");
        }

        if (response.StatusCode is HttpStatusCode.Unauthorized or HttpStatusCode.Forbidden)
        {
            throw new InvalidOperationException(
                "GitHub 更新检查未授权或达到限额，请检查 MBF_TWAIN_GITHUB_TOKEN。");
        }

        response.EnsureSuccessStatusCode();

        await using Stream contentStream = await response.Content.ReadAsStreamAsync(cancellationToken).ConfigureAwait(false);
        using JsonDocument document = await JsonDocument.ParseAsync(contentStream, cancellationToken: cancellationToken).ConfigureAwait(false);
        JsonElement root = document.RootElement;

        string tagName = GetRequiredString(root, "tag_name");
        Version latestVersion = ParseTagVersion(tagName)
            ?? throw new InvalidOperationException($"GitHub release tag is not a semantic version: {tagName}");
        string htmlUrl = GetRequiredString(root, "html_url");
        DateTimeOffset? publishedAt = TryGetDateTimeOffset(root, "published_at");

        JsonElement? installerAsset = FindInstallerAsset(root);
        Uri? installerUri = null;
        string? installerName = null;
        long? installerSize = null;
        if (installerAsset is { } asset)
        {
            installerName = GetRequiredString(asset, "name");
            installerUri = new Uri(GetRequiredString(asset, "browser_download_url"));
            installerSize = TryGetInt64(asset, "size");
        }

        return new ReleaseUpdateInfo(
            latestVersion,
            tagName,
            latestVersion.CompareTo(CurrentVersion) > 0,
            new Uri(htmlUrl),
            installerUri,
            installerName,
            installerSize,
            publishedAt);
    }

    public async Task<string> DownloadInstallerAsync(
        ReleaseUpdateInfo update,
        IProgress<DownloadProgress>? progress,
        CancellationToken cancellationToken)
    {
        if (update.InstallerUri is null || string.IsNullOrWhiteSpace(update.InstallerName))
        {
            throw new InvalidOperationException("最新 GitHub Release 没有可下载的安装包。");
        }

        string updateDirectory = Path.Combine(Path.GetTempPath(), "mbfTwain", "updates");
        Directory.CreateDirectory(updateDirectory);
        string fileName = Path.GetFileName(update.InstallerName);
        string targetPath = Path.Combine(updateDirectory, fileName);
        string partialPath = $"{targetPath}.download";

        using var request = new HttpRequestMessage(HttpMethod.Get, update.InstallerUri);
        using HttpResponseMessage response = await HttpClient.SendAsync(
            request,
            HttpCompletionOption.ResponseHeadersRead,
            cancellationToken).ConfigureAwait(false);
        response.EnsureSuccessStatusCode();

        long? totalBytes = response.Content.Headers.ContentLength ?? update.InstallerSize;
        await using Stream source = await response.Content.ReadAsStreamAsync(cancellationToken).ConfigureAwait(false);
        await using var target = new FileStream(partialPath, FileMode.Create, FileAccess.Write, FileShare.None);

        byte[] buffer = new byte[1024 * 128];
        long bytesReadTotal = 0;
        while (true)
        {
            int bytesRead = await source.ReadAsync(buffer, cancellationToken).ConfigureAwait(false);
            if (bytesRead == 0)
            {
                break;
            }

            await target.WriteAsync(buffer.AsMemory(0, bytesRead), cancellationToken).ConfigureAwait(false);
            bytesReadTotal += bytesRead;
            progress?.Report(new DownloadProgress(bytesReadTotal, totalBytes));
        }

        target.Close();
        if (File.Exists(targetPath))
        {
            File.Delete(targetPath);
        }

        File.Move(partialPath, targetPath);
        return targetPath;
    }

    private static HttpClient CreateHttpClient()
    {
        var httpClient = new HttpClient
        {
            Timeout = TimeSpan.FromSeconds(45),
        };
        httpClient.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("mbfTwain.VirtualScannerConfig", "1.0"));
        httpClient.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        httpClient.DefaultRequestHeaders.Add("X-GitHub-Api-Version", "2022-11-28");

        AuthenticationHeaderValue? authorization = GetGitHubAuthorizationHeader();
        if (authorization is not null)
        {
            httpClient.DefaultRequestHeaders.Authorization = authorization;
        }

        return httpClient;
    }

    private static AuthenticationHeaderValue? GetGitHubAuthorizationHeader()
    {
        string? token = Environment.GetEnvironmentVariable("MBF_TWAIN_GITHUB_TOKEN");
        if (string.IsNullOrWhiteSpace(token))
        {
            token = Environment.GetEnvironmentVariable("GITHUB_TOKEN");
        }

        if (string.IsNullOrWhiteSpace(token))
        {
            return null;
        }

        token = token.Trim();
        int separatorIndex = token.IndexOf(' ');
        if (separatorIndex > 0)
        {
            string scheme = token[..separatorIndex];
            string parameter = token[(separatorIndex + 1)..].Trim();
            return string.IsNullOrWhiteSpace(parameter)
                ? null
                : new AuthenticationHeaderValue(scheme, parameter);
        }

        return new AuthenticationHeaderValue("Bearer", token);
    }

    private static Version GetCurrentVersion()
    {
        Assembly assembly = Assembly.GetExecutingAssembly();
        string? informationalVersion = assembly
            .GetCustomAttribute<AssemblyInformationalVersionAttribute>()
            ?.InformationalVersion
            .Split('+')[0];

        return ParseTagVersion(informationalVersion)
            ?? assembly.GetName().Version
            ?? new Version(0, 0, 0);
    }

    private static JsonElement? FindInstallerAsset(JsonElement releaseRoot)
    {
        if (!releaseRoot.TryGetProperty("assets", out JsonElement assets) ||
            assets.ValueKind != JsonValueKind.Array)
        {
            return null;
        }

        foreach (JsonElement asset in assets.EnumerateArray())
        {
            string? name = TryGetString(asset, "name");
            string? downloadUrl = TryGetString(asset, "browser_download_url");
            if (string.IsNullOrWhiteSpace(name) || string.IsNullOrWhiteSpace(downloadUrl))
            {
                continue;
            }

            if (name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) &&
                name.Contains("Setup", StringComparison.OrdinalIgnoreCase))
            {
                return asset.Clone();
            }
        }

        return null;
    }

    private static Version? ParseTagVersion(string? tagName)
    {
        if (string.IsNullOrWhiteSpace(tagName))
        {
            return null;
        }

        string value = tagName.Trim();
        if (value.StartsWith('v') || value.StartsWith('V'))
        {
            value = value[1..];
        }

        int suffixIndex = value.IndexOfAny(['-', '+']);
        if (suffixIndex >= 0)
        {
            value = value[..suffixIndex];
        }

        string[] parts = value.Split('.');
        if (parts.Length is < 2 or > 4)
        {
            return null;
        }

        var versionParts = new int[4];
        for (int index = 0; index < versionParts.Length; index++)
        {
            versionParts[index] = 0;
        }

        for (int index = 0; index < parts.Length; index++)
        {
            if (!int.TryParse(parts[index], out versionParts[index]))
            {
                return null;
            }
        }

        return new Version(versionParts[0], versionParts[1], versionParts[2], versionParts[3]);
    }

    private static string GetRequiredString(JsonElement element, string propertyName)
    {
        return TryGetString(element, propertyName)
            ?? throw new InvalidOperationException($"GitHub response is missing `{propertyName}`.");
    }

    private static string? TryGetString(JsonElement element, string propertyName)
    {
        if (!element.TryGetProperty(propertyName, out JsonElement property) ||
            property.ValueKind is JsonValueKind.Null or JsonValueKind.Undefined)
        {
            return null;
        }

        return property.GetString();
    }

    private static long? TryGetInt64(JsonElement element, string propertyName)
    {
        if (!element.TryGetProperty(propertyName, out JsonElement property) ||
            property.ValueKind != JsonValueKind.Number)
        {
            return null;
        }

        return property.TryGetInt64(out long value) ? value : null;
    }

    private static DateTimeOffset? TryGetDateTimeOffset(JsonElement element, string propertyName)
    {
        string? value = TryGetString(element, propertyName);
        return DateTimeOffset.TryParse(value, out DateTimeOffset dateTimeOffset)
            ? dateTimeOffset
            : null;
    }
}
