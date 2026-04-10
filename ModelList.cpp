#include "ModelList.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>

// Trim whitespace from both ends of a string
static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Convert string to lowercase
static std::string toLower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return result;
}

ModelList::ModelList() {
    m_filePath = "models.txt";
}

ModelList::ModelList(const std::string& filePath)
    : m_filePath(filePath) {
    Load(filePath);
}

bool ModelList::Load(const std::string& filePath) {
    m_filePath = filePath;
    m_models.clear();

    std::ifstream file(filePath);
    if (!file.is_open()) {
        // File doesn't exist yet - that's OK, start with empty list
        return true;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);

        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;

        // Format: "username" or "username,enabled"
        bool enabled = true;
        size_t commaPos = line.find(',');
        std::string username;

        if (commaPos != std::string::npos) {
            username = trim(line.substr(0, commaPos));
            std::string enabledStr = trim(line.substr(commaPos + 1));
            enabled = (enabledStr != "0" && toLower(enabledStr) != "false" && toLower(enabledStr) != "disabled");
        } else {
            username = line;
        }

        if (!username.empty()) {
            m_models.emplace_back(username, enabled);
        }
    }

    return true;
}

bool ModelList::Save() const {
    return Save(m_filePath);
}

bool ModelList::Save(const std::string& filePath) const {
    std::ofstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "Error: Could not save model list to " << filePath << std::endl;
        return false;
    }

    file << "# SwagLive Recorder - Model Watchlist\n";
    file << "# Format: username,enabled (1=enabled, 0=disabled)\n";
    file << "# Lines starting with # are comments\n\n";

    for (const auto& model : m_models) {
        file << model.username << "," << (model.enabled ? "1" : "0") << "\n";
    }

    return true;
}

bool ModelList::AddModel(const std::string& username, bool enabled) {
    std::string lowerName = toLower(trim(username));
    if (lowerName.empty()) return false;

    // Check if already present
    for (const auto& model : m_models) {
        if (toLower(model.username) == lowerName) {
            return false; // Already in list
        }
    }

    m_models.emplace_back(username, enabled);
    return true;
}

bool ModelList::RemoveModel(const std::string& username) {
    std::string lowerName = toLower(trim(username));

    auto it = std::remove_if(m_models.begin(), m_models.end(),
        [&lowerName](const ModelEntry& e) {
            return toLower(e.username) == lowerName;
        });

    if (it == m_models.end()) return false;
    m_models.erase(it, m_models.end());
    return true;
}

bool ModelList::SetEnabled(const std::string& username, bool enabled) {
    std::string lowerName = toLower(trim(username));

    for (auto& model : m_models) {
        if (toLower(model.username) == lowerName) {
            model.enabled = enabled;
            return true;
        }
    }
    return false;
}

bool ModelList::Contains(const std::string& username) const {
    std::string lowerName = toLower(trim(username));

    for (const auto& model : m_models) {
        if (toLower(model.username) == lowerName) {
            return true;
        }
    }
    return false;
}

void ModelList::Print() const {
    if (m_models.empty()) {
        std::cout << "  (no models in watchlist)\n";
        return;
    }

    for (size_t i = 0; i < m_models.size(); ++i) {
        const auto& model = m_models[i];
        std::cout << "  " << (i + 1) << ". " << model.username
                  << " [" << (model.enabled ? "enabled" : "disabled") << "]\n";
    }
}
