// Sharp Bilinear: integer-scales each pixel, then blends only the leftover
// fraction at pixel edges. Crisp at any window size, with no uneven pixel
// widths at non-integer scales (turn Integer Scale off to fill the window).
// RetroArch single-file GLSL (cyc_shader.h). Works with either texture filter:
// the bilinear blend is done here from texel centers.

#if defined(VERTEX)

#if __VERSION__ >= 130
#define COMPAT_ATTRIBUTE in
#define COMPAT_VARYING out
#else
#define COMPAT_ATTRIBUTE attribute
#define COMPAT_VARYING varying
#endif

COMPAT_ATTRIBUTE vec4 VertexCoord;
COMPAT_ATTRIBUTE vec4 TexCoord;
COMPAT_VARYING vec2 vTex;
uniform mat4 MVPMatrix;

void main()
{
    gl_Position = MVPMatrix * VertexCoord;
    vTex = TexCoord.xy;
}

#elif defined(FRAGMENT)

#if __VERSION__ >= 130
#define COMPAT_VARYING in
#define COMPAT_TEXTURE texture
out vec4 FragColor;
#else
#define COMPAT_VARYING varying
#define COMPAT_TEXTURE texture2D
#define FragColor gl_FragColor
#endif
#ifdef GL_ES
precision mediump float;
#endif

uniform sampler2D Texture;
uniform vec2 TextureSize;
uniform vec2 InputSize;
uniform vec2 OutputSize;
COMPAT_VARYING vec2 vTex;

vec3 texel_at(vec2 t)
{
    return COMPAT_TEXTURE(Texture, (t + 0.5) / TextureSize).rgb;
}

void main()
{
    vec2 texel = vTex * InputSize;
    vec2 scale = max(floor(OutputSize / InputSize), vec2(1.0));
    vec2 region = 0.5 - 0.5 / scale;
    vec2 center = fract(texel) - 0.5;
    vec2 f = (center - clamp(center, -region, region)) * scale + 0.5;
    vec2 p = floor(texel) + f - 0.5;                 // between two texel centers
    vec2 i = floor(p);
    vec2 w = p - i;
    vec3 top = mix(texel_at(i), texel_at(i + vec2(1.0, 0.0)), w.x);
    vec3 bottom = mix(texel_at(i + vec2(0.0, 1.0)), texel_at(i + vec2(1.0, 1.0)), w.x);
    FragColor = vec4(mix(top, bottom, w.y), 1.0);
}

#endif
