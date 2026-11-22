#include "ai/Embedding.h"

#include <cmath>
#include <cstdint>

#include "common/Util.h"

namespace aegis::ai {

std::vector<float> HashingEmbedder::embed(std::string_view text) const {
    std::vector<float> vector(dimension_ == 0 ? 1 : dimension_, 0.0F);
    if (text.empty()) return vector;

    std::string lowered = to_lower(text);
    const std::string_view view(lowered);
    const std::size_t gram = view.size() < 3 ? view.size() : 3;

    for (std::size_t i = 0; i + gram <= view.size(); ++i) {
        const auto piece = view.substr(i, gram);
        const std::uint64_t hash = fnv1a64(piece);
        const std::size_t index = static_cast<std::size_t>(hash % vector.size());
        // Use a high bit to decide the sign: this keeps collisions from always
        // reinforcing each other.
        const float sign = ((hash >> 63) != 0) ? -1.0F : 1.0F;
        vector[index] += sign;
    }

    float norm = 0.0F;
    for (const float value : vector) norm += value * value;
    norm = std::sqrt(norm);
    if (norm > 0.0F) {
        for (float& value : vector) value /= norm;
    }
    return vector;
}

float cosine_similarity(std::span<const float> lhs, std::span<const float> rhs) noexcept {
    if (lhs.size() != rhs.size() || lhs.empty()) return 0.0F;

    double dot = 0.0;
    double norm_lhs = 0.0;
    double norm_rhs = 0.0;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        dot += static_cast<double>(lhs[i]) * static_cast<double>(rhs[i]);
        norm_lhs += static_cast<double>(lhs[i]) * static_cast<double>(lhs[i]);
        norm_rhs += static_cast<double>(rhs[i]) * static_cast<double>(rhs[i]);
    }
    if (norm_lhs <= 0.0 || norm_rhs <= 0.0) return 0.0F;
    return static_cast<float>(dot / (std::sqrt(norm_lhs) * std::sqrt(norm_rhs)));
}

}  // namespace aegis::ai
