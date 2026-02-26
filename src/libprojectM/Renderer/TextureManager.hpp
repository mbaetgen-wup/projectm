#pragma once

#include "Renderer/TextureSamplerDescriptor.hpp"
#include "Renderer/TextureTypes.hpp"

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace libprojectM {
namespace Renderer {

class TextureManager
{
public:
    TextureManager() = delete;

    /**
     * Constructor.
     * @param textureSearchPaths List of paths to search for textures. These paths are searched in the given order.
     */
    TextureManager(const std::vector<std::string>& textureSearchPaths);

    ~TextureManager() = default;

    /**
     * @brief Sets the current preset path to search for textures in addition to the configured paths.
     * @param path
     */
    void SetCurrentPresetPath(const std::string& path);

    /**
     * @brief Loads a texture and returns a descriptor with the given name.
     * Resets the texture age to zero.
     * @param fullName
     * @return
     */
    auto GetTexture(const std::string& fullName) -> TextureSamplerDescriptor;

    /**
     * @brief Returns a random texture descriptor, optionally using a prefix (after the `randXX_` name).
     * Will use the default texture loading logic by calling GetTexture() if a texture was selected.
     * @param randomName The filename prefix to filter. If empty, all available textures are matches. Case-insensitive.
     * @return A texture descriptor with the random texture and a default sampler, or an empty sampler if no texture could be matched.
     */
    auto GetRandomTexture(const std::string& randomName) -> TextureSamplerDescriptor;

    /**
     * @brief Returns a sampler for the given name.
     * Does not load any texture, only analyzes the prefix.
     * @param fullName The name of the sampler as used in the preset.
     * @return A sampler with the prefixed mode, or the default settings.
     */
    auto GetSampler(const std::string& fullName) -> std::shared_ptr<class Sampler>;

    /**
     * @brief Purges unused textures and increments the age counter of all stored textures.
     * Also resets the scanned texture list. Must be called exactly once per preset load.
     */
    void PurgeTextures();

    /**
     * @brief Pre-decodes an image file into CPU memory for later GPU upload.
     *
     * Thread-safe: may be called from any thread.  The decoded pixel data
     * is stored internally and used by the next GetTexture() call for this
     * name, avoiding synchronous disk I/O + image decode on the render thread.
     *
     * @param name The texture name (as referenced in the preset shader).
     * @param filePath The full path to the image file to decode.
     */
    void PreloadTextureData(const std::string& name, const std::string& filePath);

    /**
     * @brief Scans texture search paths and pre-decodes images for the given sampler names.
     *
     * Thread-safe: may be called from any thread.  Performs an independent
     * filesystem scan (does not modify the shared m_scannedTextureFiles list),
     * then decodes each matching image into CPU memory.  The decoded pixel data
     * is stored internally and consumed by the next LoadTexture() call for the
     * corresponding name, avoiding synchronous disk I/O on the render thread.
     *
     * Built-in names (main, blur1-3, noise_*, noisevol_*) and "randNN" names
     * are skipped since they don't come from disk.
     *
     * @param samplerNames Sampler names as they appear in the HLSL shader code
     *                     (e.g. "devboxb", "fw_worms", "rand00").
     */
    void PreloadTexturesForSamplers(const std::set<std::string>& samplerNames);

    /**
     * @brief Sets a callback function for loading textures from non-filesystem sources.
     * @param callback The callback function, or nullptr to disable.
     */
    void SetTextureLoadCallback(TextureLoadCallback callback);

private:
    /**
     * Texture usage statistics. Used to determine when to purge a texture.
     */
    struct UsageStats {
        UsageStats(uint32_t size)
            : sizeBytes(size){};

        uint32_t age{};       //!< Age of the texture. Represents the number of presets loaded since it was last retrieved.
        uint32_t sizeBytes{}; //!< The texture in-memory size in bytes.
    };

    /**
     * A scanned texture file on the disk.
     */
    struct ScannedFile {
        std::string filePath;          //!< Full path to the texture file
        std::string lowerCaseBaseName; //!< Texture base file name, lower case.
    };

    auto TryLoadingTexture(const std::string& name) -> TextureSamplerDescriptor;

    void Preload();

    auto LoadTexture(const ScannedFile& file) -> std::shared_ptr<Texture>;

    void AddTextureFile(const std::string& fileName, const std::string& baseName);

    static void ExtractTextureSettings(const std::string& qualifiedName, GLint& wrapMode, GLint& filterMode, std::string& name);

    void ScanTextures();

    static uint32_t TextureFormatFromChannels(int channels);

    std::vector<std::string> m_textureSearchPaths;  //!< Search paths to scan for textures.
    std::string m_currentPresetDir;                 //!< Path of the current preset to add to the search list.
    std::vector<ScannedFile> m_scannedTextureFiles; //!< The cached list with scanned texture files.
    bool m_filesScanned{false};                     //!< true if files were scanned since last preset load.

    std::shared_ptr<Texture> m_placeholderTexture;                          //!< Texture used if a requested file couldn't be found. A black 1x1 texture.
    std::map<std::string, std::shared_ptr<Texture>> m_textures;             //!< All loaded textures, including generated ones.
    std::map<std::pair<GLint, GLint>, std::shared_ptr<Sampler>> m_samplers; //!< The four sampler objects for each combination of wrap and filter modes.
    std::map<std::string, UsageStats> m_textureStats;                       //!< Map with texture stats for user-loaded files.
    std::vector<std::string> m_randomTextures;
    std::vector<std::string> m_extensions{".jpg", ".jpeg", ".dds", ".png", ".tga", ".bmp", ".dib"};

    TextureLoadCallback m_textureLoadCallback; //!< Optional callback for loading textures from non-filesystem sources.

    /**
     * @brief Pre-decoded image pixel data, ready for GPU upload.
     */
    struct PreloadedImageData {
        std::unique_ptr<unsigned char, void(*)(void*)> pixels{nullptr, free};
        int width{};
        int height{};
    };

    /// Pre-decoded textures from CPU worker.  Protected by m_preloadMutex.
    std::map<std::string, PreloadedImageData> m_preloadedTextures;
    std::mutex m_preloadMutex;
};

} // namespace Renderer
} // namespace libprojectM
