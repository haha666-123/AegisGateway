#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace aegis::ai {

inline constexpr std::size_t kEmbeddingDim = 256;

class Embedder {
public:
    virtual ~Embedder() = default;
    [[nodiscard]] virtual std::vector<float> embed(std::string_view text) const = 0;
    [[nodiscard]] virtual std::size_t dimension() const noexcept = 0;
};

// Local, dependency free text vectoriser: character 3-grams lifted into a fixed
// dimensional space by feature hashing, then L2 normalised.
//
// It is deliberately not a neural embedding model. The point is to give the
// semantic cache a deterministic, offline, testable notion of similarity (and to
// make the plumbing ready for a real embedding endpoint). Documented as such in
// the README instead of pretending it is something it is not.
class HashingEmbedder final : public Embedder {
public:
    explicit HashingEmbedder(std::size_t dimension = kEmbeddingDim) : dimension_(dimension) {}

    [[nodiscard]] std::vector<float> embed(std::string_view text) const override;
    [[nodiscard]] std::size_t dimension() const noexcept override { return dimension_; }

private:
    std::size_t dimension_;
};

[[nodiscard]] float cosine_similarity(std::span<const float> lhs,
                                      std::span<const float> rhs) noexcept;

}  // namespace aegis::ai
