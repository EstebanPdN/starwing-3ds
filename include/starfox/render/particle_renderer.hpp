#pragma once

#include "starfox/render/framebuffer.hpp"
#include "starfox/render/software_renderer.hpp"
#include "starfox/simulation/particle_system.hpp"

#include <cstdint>

// Draws the particles attached to one object (MPART.MC dots and streaks).
// Used by the 3DS frame code and host tools.
namespace starfox::render {

class ParticleRenderer {
public:
    // Draws `owner`'s live particles around owner_pose's camera-space origin,
    // interpolated at interpolation_alpha (clamped to 0-1): 2x2 dots, or a
    // line from the previous position when flag bit 2 is set. Colours are
    // colour_index_base plus the particle colour; writes are tagged three_d
    // and kept inside the pose's effect clip.
    void draw_owner(
        const simulation::ParticleSystem& particles,
        simulation::ObjectHandle owner,
        const RenderPose& owner_pose,
        double interpolation_alpha,
        Framebuffer& target,
        std::uint8_t colour_index_base = 7U * 16U) const;
private:
    static void draw_particles(std::span<const simulation::ParticleState>,simulation::ObjectHandle,
        const RenderPose&,double,Framebuffer&,std::uint8_t);
};

} // namespace starfox::render
