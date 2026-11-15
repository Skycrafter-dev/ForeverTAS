#version 440
// Small glass controls (buttons, thumbs, switches): tinted body with a lit
// rim. They sit on panels, so they do not sample the scene.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 itemSize;
    float radius;
    vec4 fillVec;       // rgb + alpha
    float rimLight;
    float lightAngle;
    float glow;         // inner glow toward the bottom (accent buttons)
    float shadowAlpha;
};

float sdRoundBox(vec2 p, vec2 b, float r)
{
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float shape(vec2 px)
{
    vec2 pad = vec2(3.0);
    vec2 size = itemSize - pad * 2.0;
    return sdRoundBox(px - itemSize * 0.5, size * 0.5, min(radius, min(size.x, size.y) * 0.5));
}

void main()
{
    vec2 px = qt_TexCoord0 * itemSize;
    float d = shape(px);
    float ds = shape(px - vec2(0.0, 1.5));
    float shadow = shadowAlpha * clamp(1.0 - ds / 3.0, 0.0, 1.0) * step(0.0, ds);

    float coverage = clamp(0.5 - d, 0.0, 1.0);
    vec2 e = vec2(0.75, 0.0);
    vec2 n = vec2(shape(px + e.xy) - shape(px - e.xy),
                  shape(px + e.yx) - shape(px - e.yx));
    n = n / max(length(n), 1e-4);
    float depth = max(-d, 0.0);
    vec2 L = vec2(cos(lightAngle), sin(lightAngle));
    float facing = dot(n, L);
    float edge = exp(-depth / 1.3);
    float soft = exp(-depth / 6.0);

    vec3 col = fillVec.rgb;
    float yN = clamp((px.y - 3.0) / max(itemSize.y - 6.0, 1.0), 0.0, 1.0);
    col += vec3(1.0) * (1.0 - yN) * 0.06;            // top sheen
    col += fillVec.rgb * glow * yN * yN * 0.6;           // bottom glow
    float spec = rimLight * (pow(max(facing, 0.0), 2.0) * (edge * 0.9 + soft * 0.15)
                             + pow(max(-facing, 0.0), 2.0) * edge * 0.35)
               + edge * 0.06;
    float a = coverage * clamp(fillVec.a + spec * 0.9, 0.0, 1.0);
    vec3 rgb = (col * fillVec.a + vec3(spec)) * coverage;
    float alpha = a + shadow * (1.0 - a);
    fragColor = vec4(rgb, alpha) * qt_Opacity;
}
