// CRT: a curved tube, beam-shaped scanlines (bright lines bloom wider) and an
// aperture-grille phosphor mask. Best at window scale 3 or more.
// RetroArch single-file GLSL (cyc_shader.h); the parameters' defaults apply.

#pragma parameter CURVATURE "Screen curvature" 0.035 0.0 0.15 0.005
#pragma parameter SHARPNESS "Horizontal sharpness" 2.0 1.0 5.0 0.25
#pragma parameter SCAN_DARK "Scanline width, dark pixels" 0.28 0.10 0.60 0.02
#pragma parameter SCAN_BRIGHT "Scanline width, bright pixels" 0.46 0.10 0.80 0.02
#pragma parameter MASK_DIM "Phosphor mask strength" 0.25 0.0 0.6 0.05
#pragma parameter BRIGHTNESS "Brightness" 1.45 1.0 2.5 0.05
#pragma parameter VIGNETTE "Corner darkening" 0.25 0.0 1.0 0.05

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

#ifdef PARAMETER_UNIFORM
uniform float CURVATURE, SHARPNESS, SCAN_DARK, SCAN_BRIGHT, MASK_DIM, BRIGHTNESS, VIGNETTE;
#else
#define CURVATURE 0.035
#define SHARPNESS 2.0
#define SCAN_DARK 0.28
#define SCAN_BRIGHT 0.46
#define MASK_DIM 0.25
#define BRIGHTNESS 1.45
#define VIGNETTE 0.25
#endif

vec2 warp(vec2 uv)
{
    uv = uv * 2.0 - 1.0;
    uv *= 1.0 + CURVATURE * (uv.yx * uv.yx);
    return uv * 0.5 + 0.5;
}

// one source row, filtered horizontally: two texel centers, a sharpened blend
vec3 row(float x, float y)
{
    float fx = x - 0.5;
    float x0 = floor(fx);
    float t = clamp((fx - x0 - 0.5) * SHARPNESS + 0.5, 0.0, 1.0);
    vec3 a = COMPAT_TEXTURE(Texture, vec2(x0 + 0.5, y) / TextureSize).rgb;
    vec3 b = COMPAT_TEXTURE(Texture, vec2(x0 + 1.5, y) / TextureSize).rgb;
    return pow(mix(a, b, t), vec3(2.4));   // to linear light
}

void main()
{
    vec2 uv = warp(vTex);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec2 px = uv * InputSize;

    // the two nearest scanlines, each a beam whose width follows its brightness
    float y0 = floor(px.y - 0.5) + 0.5;
    vec3 col = vec3(0.0);
    for (int i = 0; i < 2; ++i) {
        float y = y0 + float(i);
        vec3 c = row(px.x, y);
        float lum = max(c.r, max(c.g, c.b));
        float w = mix(SCAN_DARK, SCAN_BRIGHT, lum);
        float d = (px.y - y) / w;
        col += c * exp(-0.5 * d * d);
    }

    // aperture grille: one phosphor stripe per output column
    float m = mod(floor(gl_FragCoord.x), 3.0);
    vec3 mask = vec3(1.0 - MASK_DIM);
    if (m < 1.0) mask.r = 1.0;
    else if (m < 2.0) mask.g = 1.0;
    else mask.b = 1.0;
    col *= mask * BRIGHTNESS;

    vec2 v = uv * (1.0 - uv.yx);
    col *= mix(1.0, pow(clamp(v.x * v.y * 16.0, 0.0, 1.0), 0.25), VIGNETTE);

    FragColor = vec4(pow(clamp(col, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);
}

#endif
