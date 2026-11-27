// Defines synchronous source inspection/retrieval and its scoped ingestion boundary.
// Source pixels are never canonical terrain deliveries; conversion and jobs belong to the service.
#pragma once

#include "TerrainImage.h"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace Services::Map::Terrain
{
    /** Provides source-specific access only; callers own scheduling and retain this capability through each call. */
    class ITerrainSourceRetrieval
    {
    public:
        /** Identifies a provider-understood source without exposing borrowed native pointers. */
        struct SourceLocator
        {
            std::string Value;
        };

        /** Describes source bytes accepted only at ingestion, never canonical terrain format choices. */
        enum class SourceEncoding
        {
            Bc1,
            Rgb
        };

        /** Reports synchronous source inspection or retrieval outcome, not async retirement. */
        enum class Status
        {
            Available,
            Unavailable,
            SourceChanged,
            Failed
        };

        /** Copies source identity/layout for a later validated retrieval; owns no native or cache resources. */
        struct Inspection
        {
            Status State = Status::Unavailable;
            SourceLocator Locator;
            TerrainImage::Identity Source;
            SourceEncoding Encoding = SourceEncoding::Bc1;
            std::uint32_t WidthPixels = 0;
            std::uint32_t HeightPixels = 0;
            std::size_t RowStrideBytes = 0;
            bool SRgb = false;
        };

        /** Receives scoped source data for service-owned ingestion; implementations may not publish raw pixels. */
        class Ingestion
        {
        public:
            /** Destroys only this sink; borrowed source spans never outlive their submission call. */
            virtual ~Ingestion() = default;

            /** Accepts BC1 blocks matching source metadata; true acknowledges copied/retained ingestion data. */
            virtual bool AcceptBc1(const Inspection& source,
                std::span<const std::uint8_t> blocks) = 0;

            /** Accepts transient three-byte RGB source rows for conversion, not terrain publication.
             * True acknowledges copying/consumption inside ingestion; pixels remain borrowed during this call only.
             */
            virtual bool AcceptRgb(const Inspection& source,
                std::span<const std::uint8_t> pixels) = 0;
        };

        /** Destroys capability-local state; no service job or viewport resource is owned here. */
        virtual ~ITerrainSourceRetrieval() = default;

        /** Inspects locator identity/layout/readiness without terrain file-cache access or byte acquisition. */
        virtual Inspection Inspect(const SourceLocator& source) = 0;

        /** Revalidates expectedSource and submits its bytes to destination synchronously.
         * Available requires successful ingestion; SourceChanged/Unavailable/Failed publishes no terrain result.
         * Neither destination nor its borrowed source buffers may be retained after this call.
         */
        virtual Status Retrieve(const Inspection& expectedSource, Ingestion& destination) = 0;
    };
}
