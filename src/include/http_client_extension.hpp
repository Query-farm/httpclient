#pragma once

#include "duckdb.hpp"

namespace duckdb
{

    using HeaderMap = case_insensitive_map_t<string>;

    class HttpClientExtension : public Extension
    {
    public:
        void Load(ExtensionLoader &loader) override;
        std::string Name() override;
        std::string Version() const override;
    };

} // namespace duckdb
