// texfilter.h
//   Texture filtering for the game's own textures: anisotropic filtering, trilinear (blending
//   between mip levels) and a mip LOD bias. Applied as the game sets each texture's (or sampler
//   object's) minification filter, and re-applied to everything seen so far when the settings
//   change. Only textures the game already filters smoothly with mipmaps are changed: nearest-
//   neighbour ones are usually data (heightmaps, lookup tables) and are left exactly as they are.
#pragma once

namespace mxcam {
namespace texfilter {

// glTexParameter* / glSamplerParameter* from the game: the value the driver should get instead.
// Only GL_TEXTURE_MIN_FILTER, the max anisotropy and the LOD bias are ever changed.
float TextureParameter(unsigned target, unsigned pname, float value);
float SamplerParameter(unsigned sampler, unsigned pname, float value);

// Whether pname is one TextureParameter / SamplerParameter may change.
bool Watches(unsigned pname);

void TexturesDeleted(int count, const unsigned* textures);
void SamplersDeleted(int count, const unsigned* samplers);

// Once per frame (with a current context): re-applies everything after a settings change.
void FrameEnd();

const char* StatusText();

}  // namespace texfilter
}  // namespace mxcam
