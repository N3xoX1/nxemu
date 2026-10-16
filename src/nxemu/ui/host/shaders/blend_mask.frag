#version 450

layout(set = 0, binding = 0) uniform sampler2D tex;
layout(set = 1, binding = 0) uniform sampler2D texMask;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 finalColor;

void main()
{
    finalColor = texture(tex, fragTexCoord) * texture(texMask, fragTexCoord).a;
}
