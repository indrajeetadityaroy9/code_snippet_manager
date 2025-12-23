#pragma once

#include <dam/core_types.hpp>
#include <algorithm>
#include <set>
#include <vector>

namespace dam {

/**
 * SetOperations - Optimized set operations for FileId sets.
 *
 * Provides efficient implementations of intersection, union, and difference
 * operations with optimizations like:
 * - Sorting operands by cardinality for AND operations
 * - In-place intersection to reduce allocations
 * - Early termination on empty results
 */
class SetOperations {
public:
    /**
     * Intersect multiple sets, starting with the smallest.
     * Sets are sorted by size and intersection proceeds from smallest to largest.
     * Returns early if result becomes empty.
     *
     * @param sets Vector of sets to intersect (will be reordered)
     * @return Intersection of all sets
     */
    static std::set<FileId> intersect_all(std::vector<std::set<FileId>>& sets) {
        if (sets.empty()) {
            return {};
        }
        if (sets.size() == 1) {
            return std::move(sets[0]);
        }

        // Sort by size (smallest first) for optimal intersection order
        std::sort(sets.begin(), sets.end(),
            [](const std::set<FileId>& a, const std::set<FileId>& b) {
                return a.size() < b.size();
            });

        // Start with smallest set
        std::set<FileId> result = std::move(sets[0]);

        // Intersect with remaining sets
        for (size_t i = 1; i < sets.size() && !result.empty(); ++i) {
            intersect_in_place(result, sets[i]);
        }

        return result;
    }

    /**
     * Intersect two sets, modifying the first in-place.
     * More efficient than creating a new intersection set.
     *
     * @param result Set to modify (elements not in other will be removed)
     * @param other Set to intersect with
     */
    static void intersect_in_place(std::set<FileId>& result, const std::set<FileId>& other) {
        for (auto it = result.begin(); it != result.end(); ) {
            if (other.find(*it) == other.end()) {
                it = result.erase(it);
            } else {
                ++it;
            }
        }
    }

    /**
     * Intersect two sets, returning a new set.
     *
     * @param a First set
     * @param b Second set
     * @return Intersection of a and b
     */
    static std::set<FileId> intersect(const std::set<FileId>& a, const std::set<FileId>& b) {
        std::set<FileId> result;
        std::set_intersection(
            a.begin(), a.end(),
            b.begin(), b.end(),
            std::inserter(result, result.begin())
        );
        return result;
    }

    /**
     * Union multiple sets.
     *
     * @param sets Vector of sets to union
     * @return Union of all sets
     */
    static std::set<FileId> union_all(const std::vector<std::set<FileId>>& sets) {
        std::set<FileId> result;
        for (const auto& s : sets) {
            result.insert(s.begin(), s.end());
        }
        return result;
    }

    /**
     * Union two sets.
     *
     * @param a First set
     * @param b Second set
     * @return Union of a and b
     */
    static std::set<FileId> unite(const std::set<FileId>& a, const std::set<FileId>& b) {
        std::set<FileId> result = a;
        result.insert(b.begin(), b.end());
        return result;
    }

    /**
     * Set difference (a - b).
     *
     * @param a Set to subtract from
     * @param b Set to subtract
     * @return Elements in a but not in b
     */
    static std::set<FileId> difference(const std::set<FileId>& a, const std::set<FileId>& b) {
        std::set<FileId> result;
        std::set_difference(
            a.begin(), a.end(),
            b.begin(), b.end(),
            std::inserter(result, result.begin())
        );
        return result;
    }

    /**
     * Set difference in-place (removes elements of b from a).
     *
     * @param a Set to modify
     * @param b Elements to remove
     */
    static void difference_in_place(std::set<FileId>& a, const std::set<FileId>& b) {
        for (const auto& elem : b) {
            a.erase(elem);
        }
    }
};

}  // namespace dam
