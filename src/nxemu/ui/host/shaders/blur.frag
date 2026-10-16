#version 450

#define BLUR_SIZE 7
#define BLUR_NUM_WEIGHTS 4

layout(push_constant) uniform Constants
{
    layout(offset = 16) vec2 texCoordMin;
    vec2 texCoordMax;
    vec4 color;
    vec2 texelOffset;
    vec4 weights;
} constants;

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 finalColor;

void main()
{
    vec4 color = vec4(0.0);
    for (int i = 0; i < BLUR_SIZE; i++)
    {
        vec2 texCoord = fragTexCoord - float(i - BLUR_NUM_WEIGHTS + 1) * constants.texelOffset;
        vec2 inRegion = step(constants.texCoordMin, texCoord) * step(texCoord, constants.texCoordMax);
        color += texture(tex, texCoord) * inRegion.x * inRegion.y * constants.weights[abs(i - BLUR_NUM_WEIGHTS + 1)];
    }
    finalColor = color;
}
