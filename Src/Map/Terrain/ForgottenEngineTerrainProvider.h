// Describes externally configured FE Alternate regions and retrieves their existing BC1 artwork.
// Selection, canonical image ownership, caching and GPU presentation belong to other owners.
#pragma once
#if defined(MODLOADER_CLIENT_BUILD)
#include "Services/Map/Terrain/ITerrainDataProvider.h"

namespace MiniMapTerrainSource
{
    /** Inert, externally constructed FE1 Alternate provider. Operations require
     * admitted game-thread access outside rendering. Shared retrieval capabilities own copied
     * configuration only and may outlive this provider; none retains native or resource pointers.
     */
    class ForgottenEngineTerrainProvider final : public Services::Map::Terrain::ITerrainDataProvider
    {
    public:
        /** Identifies existing geometry candidates, not authored LEVEL labels or physical Z regions. */
        enum class Candidate
        {
            CandidateZ5074 = 0,
            CandidateZ6274 = 1,
            CandidateZ7474 = 2
        };

        /** Associates a physical region with optional existing artwork, not an independent selector.
         * Missing artwork retains coverage identity and describes unavailable imagery.
         */
        struct RegionSource
        {
            Region Coverage;
            std::optional<Candidate> Artwork;
        };

        /** Copied application identity. WorldId is the actual SDK world name; WorldGeneration
         * is MiniMapMap's generation. InstanceId must be ForgottenEngine1. ContentRevision
         * identifies the embedded artwork and must change when its bytes change. Construction
         * rejects absent identities, unsupported candidates and unsafe/overlong collection names.
         */
        struct Configuration
        {
            std::string WorldId;
            std::uint64_t WorldGeneration = 0;
            std::string InstanceId;
            std::string ContentRevision;
            std::vector<RegionSource> Regions;
        };

        /** Copies and validates values; throws invalid_argument without IO or native/resource access. */
        explicit ForgottenEngineTerrainProvider(Configuration configuration);
        /** Returns the configured Alternate coverage; performs no position-based selection/registration. */
        Coverage DescribeCoverage() const override;
        /** Maps the service-selected RegionId to its configured artwork in the admitted session.
         * Returns unavailable on mismatched world/region/session; does not classify either query pose.
         */
        SourceDescription DescribeSource(const SourceQuery& query) override;
        /** Returns exact artwork placement for application demand; never physical occupancy. */
        static Services::Map::Terrain::TerrainImage::Registration GetRegistration();

    private:
        /** Independent synchronous capability. Only immutable configuration survives an operation;
         * module resource spans exist on the call stack until ingestion returns.
         */
        class SourceRetrieval final : public Services::Map::Terrain::ITerrainSourceRetrieval
        {
        public:
            /** Copies the already validated provider configuration; performs no resource/native access. */
            SourceRetrieval(Configuration configuration, Candidate candidate);
            /** Resolves RCDATA and exact byte length, returning copied metadata and session status. */
            Inspection Inspect(const SourceLocator& source) override;
            /** Revalidates complete evidence and synchronously submits borrowed blocks to AcceptBc1.
             * Rechecks session admission after ingestion, which may reenter application lifecycle.
             */
            Status Retrieve(const Inspection& expectedSource, Ingestion& destination) override;
        private:
            /** Checks resource metadata; a nonnull bytes output additionally borrows data for retrieval.
             * A null output performs no LoadResource/LockResource. Missing/malformed content is Failed.
             */
            Status InspectResource(const SourceLocator& source, Inspection& inspection,
                std::span<const std::uint8_t>* bytes) const;
            /** Compares every inspection field, including exact locator, layout and logical page. */
            static bool SameInspection(const Inspection& left, const Inspection& right) noexcept;
            const Configuration configuration_;
            const Candidate candidate_;
        };

        /** Validates identity strings, the single supported instance and candidate without native calls. */
        static void ValidateConfiguration(const Configuration& configuration);
        /** Returns table index 0..2 or throws invalid_argument for unsupported enum values. */
        static std::size_t GetCandidateIndex(Candidate candidate);
        /** Returns fixed candidate identity, independent of current character/floor state. */
        static const char* GetCandidateId(Candidate candidate);
        /** Returns the selected BC1 resource ID, never one of the legacy PNG resource IDs. */
        static int GetResourceId(Candidate candidate);
        /** Returns opaque world/instance/candidate identity and artwork plus conversion-policy revision. */
        static Services::Map::Terrain::TerrainImage::Identity GetIdentity(
            const Configuration& configuration, Candidate candidate);
        /** Returns a valid candidate-order collection with page (0,0), unrelated to world placement. */
        static Services::Map::Terrain::TerrainPageIdentity GetPageIdentity(
            const Configuration& configuration, Candidate candidate);
        /** Builds immutable expected packed BC1 source metadata without resource acquisition. */
        static Services::Map::Terrain::ITerrainSourceRetrieval::Inspection DescribeInspection(
            const Configuration& configuration, Candidate candidate);
        /** Checks game-thread admission first; stale generation/name returns SourceChanged,
         * closed admission or unavailable native/world access returns Unavailable.
         */
        static Services::Map::Terrain::ITerrainSourceRetrieval::Status GetSessionStatus(
            const Configuration& configuration);

        static int resourceModuleAnchor_;
        const Configuration configuration_;
    };
}
#endif
