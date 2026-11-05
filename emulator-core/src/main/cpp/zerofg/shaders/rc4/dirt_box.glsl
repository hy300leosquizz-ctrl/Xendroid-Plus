// RC4 slim box9 Compat oracle: five bilinear samples per axis reconstruct the
// uniform nine-texel weights exactly in real arithmetic, including CLAMP_TO_EDGE.
// The existing box5 loop stays in dirt_slim.comp so its arithmetic is unchanged.
#ifndef ZFG_DIRT_BOX_GLSL
#define ZFG_DIRT_BOX_GLSL

vec3 DirtBox9Compat(sampler2D picture, vec2 centre, ivec2 extent) {
  const float off[5] = float[5](-3.5, -1.5, 0.0, 1.5, 3.5);
  const float wt[5] = float[5](2.0 / 9.0, 2.0 / 9.0, 1.0 / 9.0, 2.0 / 9.0, 2.0 / 9.0);
  vec3 sum = vec3(0.0);
  for (int j = 0; j < 5; ++j)
    for (int k = 0; k < 5; ++k)
      sum += wt[j] * wt[k] * texture(picture, (centre + vec2(off[k], off[j])) / vec2(extent)).rgb;
  return sum;
}

#endif
