#version 330 core

// ── From vertex shader ───────────────────────────────────────────────────────
in vec3 v_fragPos;
in vec3 v_normal;
in vec2 v_texCoord;

// ── Material ─────────────────────────────────────────────────────────────────
uniform vec3  u_matAmbient;
uniform vec3  u_matDiffuse;
uniform vec3  u_matSpecular;
uniform float u_shininess;        // specular exponent (e.g. 32.0, 64.0, 128.0)

// ── Directional light ────────────────────────────────────────────────────────
// u_dirLightDir: unit vector pointing FROM the surface TOWARD the light source
uniform vec3  u_dirLightDir;
uniform vec3  u_dirLightAmbient;
uniform vec3  u_dirLightDiffuse;
uniform vec3  u_dirLightSpecular;

// ── Point light ───────────────────────────────────────────────────────────────
uniform vec3  u_pointLightPos;
uniform vec3  u_pointLightAmbient;
uniform vec3  u_pointLightDiffuse;
uniform vec3  u_pointLightSpecular;
// Attenuation: 1 / (c + l*d + q*d^2)  — typical defaults: 1.0, 0.09, 0.032
uniform float u_attenConst;
uniform float u_attenLinear;
uniform float u_attenQuadratic;

// ── Camera / output ───────────────────────────────────────────────────────────
uniform vec3  u_viewPos;
uniform float u_gamma;            // gamma exponent for correction (typically 2.2)

out vec4 fragColor;

// ── Directional light contribution ───────────────────────────────────────────
vec3 calcDirectional(vec3 N, vec3 V)
{
    vec3 L = normalize(u_dirLightDir);    // caller supplies a unit vec; renorm for safety
    vec3 R = reflect(-L, N);

    vec3 ambient  = u_dirLightAmbient  * u_matAmbient;

    float NdotL   = max(dot(N, L), 0.0);
    vec3 diffuse  = u_dirLightDiffuse  * u_matDiffuse  * NdotL;

    float RdotV   = max(dot(R, V), 0.0);
    vec3 specular = u_dirLightSpecular * u_matSpecular  * pow(RdotV, u_shininess);

    return ambient + diffuse + specular;
}

// ── Point light contribution ──────────────────────────────────────────────────
vec3 calcPoint(vec3 N, vec3 V)
{
    vec3  toLight = u_pointLightPos - v_fragPos;
    vec3  L       = normalize(toLight);
    vec3  R       = reflect(-L, N);
    float dist    = length(toLight);

    // Quadratic attenuation
    float att = 1.0 / (u_attenConst
                     + u_attenLinear    * dist
                     + u_attenQuadratic * dist * dist);

    vec3 ambient  = u_pointLightAmbient  * u_matAmbient;

    float NdotL   = max(dot(N, L), 0.0);
    vec3 diffuse  = u_pointLightDiffuse  * u_matDiffuse  * NdotL;

    float RdotV   = max(dot(R, V), 0.0);
    vec3 specular = u_pointLightSpecular * u_matSpecular  * pow(RdotV, u_shininess);

    return att * (ambient + diffuse + specular);
}

// ── Main ──────────────────────────────────────────────────────────────────────
void main()
{
    vec3 N = normalize(v_normal);              // renormalise after interpolation
    vec3 V = normalize(u_viewPos - v_fragPos); // view direction

    vec3 color = calcDirectional(N, V)
               + calcPoint(N, V);

    // Gamma correction: linearise the render into display space
    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / u_gamma));

    fragColor = vec4(color, 1.0);
}
