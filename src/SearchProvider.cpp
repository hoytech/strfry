#include "search/SearchProvider.h"
#include "search/NoopSearchProvider.h"
#include "search/LmdbSearchProvider.h"
#include "golpe.h"


std::unique_ptr<ISearchProvider> makeSearchProvider() {
    // Check if search is enabled
    if (!cfg().relay__search__enabled) {
        return std::make_unique<NoopSearchProvider>();
    }

    // Check backend configuration
    std::string backend = cfg().relay__search__backend;

    // Explicit noop request
    if (backend == "noop") {
        return std::make_unique<NoopSearchProvider>();
    }

    // Default to LMDB (natural choice for strfry)
    if (backend == "lmdb" || backend.empty()) {
        return std::make_unique<LmdbSearchProvider>();
    }

    // Unknown backend - log warning and fall back to lmdb
    LW << "Unknown search backend: " << backend << ", falling back to lmdb";
    return std::make_unique<LmdbSearchProvider>();
}
