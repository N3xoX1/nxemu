#version 450

#define MAX_NUM_STOPS 16
#define LINEAR 0
#define RADIAL 1
#define CONIC 2
#define REPEATING_LINEAR 3
#define REPEATING_RADIAL 4
#define REPEATING_CONIC 5
#define PI 3.14159265

layout(set = 0, binding = 0, std140) uniform Gradient
{
    int func;      // one of the above definitions
    int numStops;
    vec2 p;        // linear: starting point,         radial: center,                        conic: center
    vec2 v;        // linear: vector to ending point, radial: 2d curvature (inverse radius), conic: angled unit vector
    vec4 stopColors[MAX_NUM_STOPS];
    vec4 stopPositions[MAX_NUM_STOPS]; // x only; normalized, 0 -> starting point, 1 -> ending point
} gradient;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;
layout(location = 0) out vec4 finalColor;

vec4 MixStopColors(float t)
{
    vec4 color = gradient.stopColors[0];
    for (int i = 1; i < gradient.numStops; i++)
    {
        color = mix(color, gradient.stopColors[i], smoothstep(gradient.stopPositions[i - 1].x, gradient.stopPositions[i].x, t));
    }
    return color;
}

void main()
{
    float t = 0.0;
    if (gradient.func == LINEAR || gradient.func == REPEATING_LINEAR)
    {
        t = dot(gradient.v, fragTexCoord - gradient.p) / dot(gradient.v, gradient.v);
    }
    else if (gradient.func == RADIAL || gradient.func == REPEATING_RADIAL)
    {
        t = length(gradient.v * (fragTexCoord - gradient.p));
    }
    else if (gradient.func == CONIC || gradient.func == REPEATING_CONIC)
    {
        mat2 rotation = mat2(gradient.v.x, -gradient.v.y, gradient.v.y, gradient.v.x);
        vec2 rotated = rotation * (fragTexCoord - gradient.p);
        t = 0.5 + atan(-rotated.x, rotated.y) / (2.0 * PI);
    }

    if (gradient.func == REPEATING_LINEAR || gradient.func == REPEATING_RADIAL || gradient.func == REPEATING_CONIC)
    {
        float t0 = gradient.stopPositions[0].x;
        float t1 = gradient.stopPositions[gradient.numStops - 1].x;
        t = t0 + mod(t - t0, t1 - t0);
    }

    finalColor = fragColor * MixStopColors(t);
}
