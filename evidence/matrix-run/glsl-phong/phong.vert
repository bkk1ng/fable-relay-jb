#version 330 core

// ── Vertex attributes ─────────────────────────────────────────────────────────
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_texCoord;

// ── Transforms ───────────────────────────────────────────────────────────────
uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_projection;
// transpose(inverse(mat3(u_model))) — corrects non-uniform scale on normals
uniform mat3 u_normalMatrix;

// ── To fragment shader ───────────────────────────────────────────────────────
out vec3 v_fragPos;    // world-space fragment position
out vec3 v_normal;     // world-space normal (normalised)
out vec2 v_texCoord;

void main()
{
    vec4 worldPos = u_model * vec4(a_position, 1.0);

    v_fragPos  = worldPos.xyz;
    v_normal   = normalize(u_normalMatrix * a_normal);
    v_texCoord = a_texCoord;

    gl_Position = u_projection * u_view * worldPos;
}
