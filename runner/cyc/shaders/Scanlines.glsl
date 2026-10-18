// Scanlines: crisp pixels with a dark gap between source rows, brightness
// compensated. Flat screen, no mask.
// RetroArch single-file GLSL (cyc_shader.h); the parameters' defaults apply.

#pragma parameter STRENGTH "Scanline darkness" 0.55 0.0 1.0 0.05
#pragma parameter BRIGHTNESS "Brightness" 1.25 1.0 2.0 0.05

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
COMPAT_VARYING vec2 vTex;

#ifdef PARAMETER_UNIFORM
uniform float STRENGTH, BRIGHTNESS;
#else
#define STRENGTH 0.55
#define BRIGHTNESS 1.25
#endif

void main()
{
    vec2 px = vTex * InputSize;
    vec3 c = COMPAT_TEXTURE(Texture, (floor(px) + 0.5) / TextureSize).rgb;
    float d = abs(fract(px.y) - 0.5) * 2.0;          // 0 at the row's center, 1 at its edge
    float scan = 1.0 - STRENGTH * smoothstep(0.35, 1.0, d);
    FragColor = vec4(clamp(c * scan * BRIGHTNESS, 0.0, 1.0), 1.0);
}

#endif
