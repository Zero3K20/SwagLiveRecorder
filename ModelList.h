#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <string>
#include <vector>
#include <set>

// Represents a model entry in the watchlist
struct ModelEntry {
    std::string username;   // Model's username/slug on swag.live
    bool enabled;           // Whether auto-recording is enabled for this model

    ModelEntry() : enabled(true) {}
    ModelEntry(const std::string& name, bool en = true)
        : username(name), enabled(en) {}
};

// Manages the persistent watchlist of models to auto-record
class ModelList {
public:
    ModelList();
    explicit ModelList(const std::string& filePath);

    // Load watchlist from file
    bool Load(const std::string& filePath);

    // Save watchlist to file
    bool Save() const;
    bool Save(const std::string& filePath) const;

    // Add a model to the watchlist (returns false if already present)
    bool AddModel(const std::string& username, bool enabled = true);

    // Remove a model from the watchlist (returns false if not found)
    bool RemoveModel(const std::string& username);

    // Enable or disable recording for a model
    bool SetEnabled(const std::string& username, bool enabled);

    // Check if a model is in the watchlist
    bool Contains(const std::string& username) const;

    // Get all models
    const std::vector<ModelEntry>& GetModels() const { return m_models; }

    // Get number of models
    size_t Count() const { return m_models.size(); }

    // Print the watchlist to stdout
    void Print() const;

private:
    std::vector<ModelEntry> m_models;
    std::string m_filePath;
};
