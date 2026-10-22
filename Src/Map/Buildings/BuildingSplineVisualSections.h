#pragma once
#include "Native/VisualAccessCore.h"

// Pure owned descriptor preparation. Prepared never implies drawable mesh
// geometry: vertex/index data, deformation and material evaluation are absent.
namespace MiniMapBuildingSplineVisualSections
{
    namespace V = MiniMapVisualAccess;
    enum class State : uint8_t { Prepared, Incomplete, Unsupported, InvalidSource };
    enum class Representation : uint8_t { MeshDescriptor, PathSource, Unsupported };
    struct Section
    {
        // Reuse the complete copied descriptor, including effective material
        // slots, identity, transform, spline parameters and visibility flags.
        V::Component Descriptor;
        State Status=State::Unsupported;
        Representation Type=Representation::Unsupported;
    };
    struct Result
    {
        // Provenance and actor visibility stay in the immutable Observation
        // owner paired with this result by BuildingVisuals.
        State Status=State::InvalidSource;
        std::vector<Section> Sections;
    };
    inline Result Prepare(const V::Observation& source)
    {
        if(source.State!=V::Status::Ready || !source.EnumerationComplete ||
            source.Components.size()>V::MaximumComponents)return {};
        // Reject the entire source before copying any partial descriptors.
        for(const auto& component:source.Components)
            if(component.Materials.size()>V::MaximumMaterials ||
                !V::Valid(component.ComponentToWorld) ||
                (component.SplineMesh && !V::Valid(*component.SplineMesh)))return {};
        Result result;result.Status=State::Prepared;
        result.Sections.reserve(source.Components.size());
        for(const auto& component:source.Components)
        {
            Section section;section.Descriptor=component;
            switch(component.Type)
            {
            case V::Kind::SplineMesh:
                section.Type=Representation::MeshDescriptor;
                section.Status=component.SplineMesh?State::Prepared:State::Incomplete;
                break;
            case V::Kind::StaticMesh:
                section.Type=Representation::MeshDescriptor;
                section.Status=State::Prepared;
                break;
            case V::Kind::InstancedStaticMesh:
                section.Type=Representation::MeshDescriptor;
                section.Status=State::Incomplete; // No copied instance transforms.
                break;
            case V::Kind::Spline:
                section.Type=Representation::PathSource;
                section.Status=State::Incomplete; // No component-local path copied.
                break;
            case V::Kind::Other:
            case V::Kind::UnsupportedPrimitive:
            case V::Kind::UnsupportedScene:
            default:
                section.Type=Representation::Unsupported;
                section.Status=State::Unsupported;
                break;
            }
            if(section.Type==Representation::MeshDescriptor)
            {
                if(component.Mesh.Index<0 || component.Mesh.Serial<=0)
                    section.Status=State::Incomplete;
                for(const auto& material:component.Materials)
                    if(material.Effective.Index<0 || material.Effective.Serial<=0)
                        section.Status=State::Incomplete;
            }
            result.Sections.push_back(std::move(section));
        }
        return result;
    }
}
