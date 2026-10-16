#version 450

layout(push_constant) uniform Constants
{
    mat4 transform;
    vec2 translate;
} constants;

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec4 fragColor;

void main()
{
    fragTexCoord = inTexCoord;
    fragColor = inColor;
    gl_Position = constants.transform * vec4(inPosition + constants.translate, 0.0, 1.0);
}
