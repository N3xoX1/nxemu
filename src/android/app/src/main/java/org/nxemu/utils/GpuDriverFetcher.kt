package org.nxemu.utils

import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.GLES20
import org.json.JSONArray
import org.json.JSONObject
import org.nxemu.NxEmuApplication
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.time.Instant
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

object GpuDriverFetcher {
    private const val CACHE_MS = 15 * 60 * 1000L
    private const val SETTLE_MS = 7 * 24 * 60 * 60 * 1000L
    private const val MAX_DRIVER_BYTES = 256L * 1024 * 1024
    private const val MAX_NOTES = 1500
    private const val USER_AGENT = "NxEmu-Android"

    private class Repo(
        val name: String,
        val path: String,
        val useTagName: Boolean = false,
        val sortByPublishTime: Boolean = false,
        val titleNoise: Regex? = null,
    )

    private val adpkgSuffix = Regex("""\.adpkg$""", RegexOption.IGNORE_CASE)

    private class Recommendation(val models: IntRange, val repo: Int, val name: String, val asset: Regex?)

    private val repos = listOf(
        // Release names are file names like "turnip_mrpurple_T30-toasted.adpkg"; show "T30"
        Repo("Mr. Purple Turnip", "MrPurple666/purple-turnip",
            titleNoise = Regex("""^v?turnip[-_]mrpurple[-_]|[-_]toasted""", RegexOption.IGNORE_CASE)),
        Repo("GameHub Adreno 8xx", "crueter/GameHub-8Elite-Drivers"),
        Repo("KIMCHI Turnip", "K11MCH1/AdrenoToolsDrivers", useTagName = true, sortByPublishTime = true),
        Repo("Weab-Chan Freedreno", "Weab-chan/freedreno_turnip-CI"),
        Repo("Whitebelyash Turnip", "whitebelyash/freedreno_turnip-CI", sortByPublishTime = true),
    )

    // The newest stable release that is at least a week old, from the source that suits
    // the GPU family. The asset regex picks which zip to use; a null regex takes the first zip.
    private val kimchiTurnip = Regex("""^turnip_v[\d.]+_r\d+\.zip$""", RegexOption.IGNORE_CASE)
    private val recommendations = listOf(
        Recommendation(10..99, 2, "KIMCHI", kimchiTurnip),
        Recommendation(600..799, 0, "Mr. Purple", null),
        Recommendation(800..899, 1, "GameHub", null),
    )

    private val downloading = AtomicBoolean(false)

    @Volatile
    private var gpuRenderer: String? = null

    @Volatile
    private var catalog: JSONObject? = null

    @Volatile
    private var catalogTime = 0L

    fun gpuModel(): String {
        gpuRenderer?.let { return it }
        val renderer = try {
            queryGlRenderer()
        } catch (_: Exception) {
            ""
        }
        gpuRenderer = renderer
        return renderer
    }

    fun recommendedLabel(): String {
        return recommendationFor(adrenoModel(gpuModel()))?.let { "${it.name} (latest)" }.orEmpty()
    }

    fun catalogJson(force: Boolean): String {
        val cached = catalog
        if (!force && cached != null && System.currentTimeMillis() - catalogTime < CACHE_MS) {
            return cached.toString()
        }
        val pool = Executors.newFixedThreadPool(repos.size)
        val groups = try {
            repos.mapIndexed { index, repo -> pool.submit<JSONObject> { fetchGroup(index, repo) } }
                .map { it.get(30, TimeUnit.SECONDS) }
        } finally {
            pool.shutdownNow()
        }
        val renderer = gpuModel()
        val result = JSONObject()
            .put("gpu", renderer)
            .put("supported", GpuDriverHelper.supportsCustomDrivers())
            .put("groups", JSONArray(groups))
        recommendedAsset(adrenoModel(renderer), groups)?.let { result.put("recommended", it) }
        catalog = result
        catalogTime = System.currentTimeMillis()
        return result.toString()
    }

    fun download(repoPath: String, tag: String, assetName: String, progress: (Int) -> Unit): String {
        val asset = findAsset(repoPath, tag, assetName) ?: return "missing"
        if (!downloading.compareAndSet(false, true)) {
            return "busy"
        }
        val temp = File(NxEmuApplication.appContext.cacheDir, "gpu_driver_download.zip")
        try {
            val url = asset.getString("url")
            if (!url.startsWith("https://github.com/")) {
                return "missing"
            }
            downloadTo(url, temp, progress)
            return GpuDriverHelper.importDownloaded(temp, asset.getString("file"))
        } catch (_: Exception) {
            return "network"
        } finally {
            temp.delete()
            downloading.set(false)
        }
    }

    private fun findAsset(repoPath: String, tag: String, assetName: String): JSONObject? {
        val groups = catalog?.optJSONArray("groups") ?: return null
        for (g in 0 until groups.length()) {
            val group = groups.getJSONObject(g)
            if (group.optString("repo") != repoPath) {
                continue
            }
            val releases = group.optJSONArray("releases") ?: return null
            for (r in 0 until releases.length()) {
                val release = releases.getJSONObject(r)
                if (release.optString("tag") != tag) {
                    continue
                }
                val assets = release.optJSONArray("assets") ?: return null
                for (a in 0 until assets.length()) {
                    val asset = assets.getJSONObject(a)
                    if (asset.optString("name") == assetName) {
                        return asset
                    }
                }
            }
        }
        return null
    }

    private fun downloadTo(url: String, dest: File, progress: (Int) -> Unit) {
        val connection = openConnection(url, "application/octet-stream")
        try {
            if (connection.responseCode != HttpURLConnection.HTTP_OK) {
                throw IOException("HTTP ${connection.responseCode}")
            }
            val total = connection.contentLengthLong
            if (total > MAX_DRIVER_BYTES) {
                throw IOException("too large")
            }
            connection.inputStream.use { input ->
                dest.outputStream().use { output ->
                    val buffer = ByteArray(64 * 1024)
                    var written = 0L
                    var lastPercent = -1
                    while (true) {
                        val read = input.read(buffer)
                        if (read < 0) {
                            break
                        }
                        written += read
                        if (written > MAX_DRIVER_BYTES) {
                            throw IOException("too large")
                        }
                        output.write(buffer, 0, read)
                        if (total > 0) {
                            val percent = (written * 100 / total).toInt()
                            if (percent != lastPercent) {
                                lastPercent = percent
                                progress(percent)
                            }
                        }
                    }
                }
            }
        } finally {
            connection.disconnect()
        }
    }

    private fun fetchGroup(index: Int, repo: Repo): JSONObject {
        val group = JSONObject()
            .put("name", repo.name)
            .put("repo", repo.path)
        val cacheFile = File(NxEmuApplication.appContext.cacheDir, "gpu_driver_releases_$index.json")
        val body = try {
            fetchText("https://api.github.com/repos/${repo.path}/releases?per_page=100").also {
                cacheFile.writeText(it)
            }
        } catch (e: Exception) {
            if (!cacheFile.isFile) {
                return group.put("releases", JSONArray()).put("error", errorText(e))
            }
            try {
                cacheFile.readText().also { group.put("stale", true) }
            } catch (_: Exception) {
                return group.put("releases", JSONArray()).put("error", errorText(e))
            }
        }
        return try {
            group.put("releases", parseReleases(repo, JSONArray(body)))
        } catch (e: Exception) {
            group.put("releases", JSONArray()).put("error", "Unexpected response")
        }
    }

    private fun parseReleases(repo: Repo, json: JSONArray): JSONArray {
        val releases = ArrayList<JSONObject>()
        for (i in 0 until json.length()) {
            val node = json.optJSONObject(i) ?: continue
            if (node.optBoolean("draft")) {
                continue
            }
            val tag = node.optString("tag_name")
            val name = node.optString("name").takeIf { it.isNotBlank() && it != "null" } ?: tag
            val assets = JSONArray()
            node.optJSONArray("assets")?.let { list ->
                for (a in 0 until list.length()) {
                    val asset = list.optJSONObject(a) ?: continue
                    val assetName = asset.optString("name")
                    val url = asset.optString("browser_download_url")
                    if (!assetName.endsWith(".zip", ignoreCase = true) || url.isEmpty()) {
                        continue
                    }
                    assets.put(JSONObject()
                        .put("name", assetName)
                        .put("url", url)
                        .put("size", asset.optLong("size"))
                        .put("file", GpuDriverHelper.storedName(repo.path.substringBefore('/') + "-" + tag + "-" + assetName)))
                }
            }
            if (assets.length() == 0) {
                continue
            }
            val published = node.optString("published_at")
            releases.add(JSONObject()
                .put("title", displayTitle(repo, if (repo.useTagName) tag else name))
                .put("tag", tag)
                .put("published", published)
                .put("time", parseTime(published))
                .put("prerelease", node.optBoolean("prerelease"))
                .put("notes", node.optString("body").takeIf { it != "null" }.orEmpty().take(MAX_NOTES))
                .put("assets", assets))
        }
        if (repo.sortByPublishTime) {
            releases.sortByDescending { it.optLong("time") }
        }
        releases.firstOrNull { !it.optBoolean("prerelease") }?.put("latest", true)
        return JSONArray(releases)
    }

    private fun recommendedAsset(model: Int, groups: List<JSONObject>): JSONObject? {
        val rec = recommendationFor(model) ?: return null
        val group = groups[rec.repo]
        val releases = group.optJSONArray("releases") ?: return null
        val result = JSONObject().put("label", "${rec.name} (latest)").put("group", group.optString("name"))
        // Stable releases with a usable zip, newest first
        val candidates = ArrayList<Pair<JSONObject, JSONObject>>()
        for (i in 0 until releases.length()) {
            val release = releases.getJSONObject(i)
            if (release.optBoolean("prerelease")) {
                continue
            }
            val assets = release.getJSONArray("assets")
            for (a in 0 until assets.length()) {
                val candidate = assets.getJSONObject(a)
                if (rec.asset == null || rec.asset.containsMatchIn(candidate.optString("name"))) {
                    candidates.add(release to candidate)
                    break
                }
            }
        }
        // Give new releases time to get a hotfix before recommending them
        val settled = System.currentTimeMillis() - SETTLE_MS
        val (pick, asset) = candidates.firstOrNull { it.first.optLong("time") <= settled }
            ?: candidates.firstOrNull()
            ?: return result
        return result
            .put("label", "${rec.name} ${pick.optString("title")}")
            .put("repo", group.optString("repo"))
            .put("release", pick.optString("title"))
            .put("published", pick.optString("published"))
            .put("tag", pick.optString("tag"))
            .put("asset", asset.optString("name"))
            .put("size", asset.optLong("size"))
            .put("file", asset.optString("file"))
    }

    private fun displayTitle(repo: Repo, raw: String): String {
        var title = raw.trim().replace(adpkgSuffix, "")
        repo.titleNoise?.let { title = title.replace(it, "") }
        return title.trim().ifEmpty { raw }
    }

    private fun recommendationFor(model: Int): Recommendation? {
        return recommendations.firstOrNull { model in it.models }
    }

    // GL_RENDERER looks like "Adreno (TM) 740", or "Adreno (TM) A32" on some handhelds
    private fun adrenoModel(renderer: String): Int {
        val match = Regex("""Adreno\s*(?:\(TM\))?\s*A?(\d+)""", RegexOption.IGNORE_CASE).find(renderer)
        return match?.groupValues?.get(1)?.toIntOrNull() ?: 0
    }

    private fun parseTime(text: String): Long {
        return try {
            Instant.parse(text).toEpochMilli()
        } catch (_: Exception) {
            0L
        }
    }

    private fun errorText(e: Exception): String {
        val message = e.message.orEmpty()
        return when {
            message.contains("HTTP 403") || message.contains("HTTP 429") -> "GitHub rate limit reached. Try again later."
            message.startsWith("HTTP ") -> "GitHub returned $message"
            else -> "Couldn't reach GitHub"
        }
    }

    private fun fetchText(url: String): String {
        val connection = openConnection(url, "application/vnd.github+json")
        try {
            if (connection.responseCode != HttpURLConnection.HTTP_OK) {
                throw IOException("HTTP ${connection.responseCode}")
            }
            return connection.inputStream.use { it.readBytes().toString(Charsets.UTF_8) }
        } finally {
            connection.disconnect()
        }
    }

    private fun openConnection(url: String, accept: String): HttpURLConnection {
        return (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 15_000
            readTimeout = 30_000
            instanceFollowRedirects = true
            setRequestProperty("Accept", accept)
            setRequestProperty("User-Agent", USER_AGENT)
        }
    }

    private fun queryGlRenderer(): String {
        val display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        if (display == EGL14.EGL_NO_DISPLAY) {
            return ""
        }
        val version = IntArray(2)
        if (!EGL14.eglInitialize(display, version, 0, version, 1)) {
            return ""
        }
        val configAttribs = intArrayOf(
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
            EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT,
            EGL14.EGL_NONE,
        )
        val configs = arrayOfNulls<EGLConfig>(1)
        val count = IntArray(1)
        if (!EGL14.eglChooseConfig(display, configAttribs, 0, configs, 0, 1, count, 0) || count[0] == 0) {
            return ""
        }
        val config = configs[0] ?: return ""
        val context = EGL14.eglCreateContext(
            display, config, EGL14.EGL_NO_CONTEXT,
            intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0,
        )
        if (context == EGL14.EGL_NO_CONTEXT) {
            return ""
        }
        val surface = EGL14.eglCreatePbufferSurface(
            display, config, intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE), 0,
        )
        try {
            if (surface == EGL14.EGL_NO_SURFACE || !EGL14.eglMakeCurrent(display, surface, surface, context)) {
                return ""
            }
            return GLES20.glGetString(GLES20.GL_RENDERER).orEmpty()
        } finally {
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
            if (surface != EGL14.EGL_NO_SURFACE) {
                EGL14.eglDestroySurface(display, surface)
            }
            EGL14.eglDestroyContext(display, context)
        }
    }
}
