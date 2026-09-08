#version 460
#extension GL_EXT_ray_tracing : require

// Reaching this shader means the shadow ray hit nothing, so the surface is
// lit. The payload starts at 0 (occluded) and is only raised here, which is
// what makes RayFlags TERMINATE_ON_FIRST_HIT safe to use.
layout(location = 1) rayPayloadInEXT float shadowVisibility;

void main()
{
    shadowVisibility = 1.0;
}
