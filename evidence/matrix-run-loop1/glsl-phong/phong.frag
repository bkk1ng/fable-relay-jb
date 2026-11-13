#version 330 core

// ---------------------------------------------------------------------------
// Inputs from vertex shader
// ---------------------------------------------------------------------------
in vec3 vFragPos;
in vec3 vNormal;
in vec2 vTexCoord;

// ---------------------------------------------------------------------------
// Material
// ---------------------------------------------------------------------------
uniform vec3  uAmbientColor;       // ka
uniform vec3  uDiffuseColor;       // kd
uniform vec3  uSpecularColor;      // ks
uniform float uShininess;          // Phong exponent

// ---------------------------------------------------------------------------
// Directional light  (direction points TOWARD the light, world-space)
// ---------------------------------------------------------------------------
uniform vec3  uDirLightDir;        // normalized
uniform vec3  uDirLightAmbient;
uniform vec3  uDirLightDiffuse;
uniform vec3  uDirLightSpecular;

// ---------------------------------------------------------------------------
// Point light
// ---------------------------------------------------------------------------
uniform vec3  uPointLightPos;      // world-space
uniform vec3  uPointLightAmbient;
uniform vec3  uPointLightDiffuse;
uniform vec3  uPointLightSpecular;
uniform float uConstantAtten;      // k0
uniform float uLinearAtten;        // k1
uniform float uQuadraticAtten;     // k2

// ---------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------
uniform vec3  uViewPos;            // camera position, world-space

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------
out vec4 FragColor;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Apply gamma correction (linear -> sRGB, gamma ≈ 2.2)
vec3 gammaCorrect(vec3 linearColor) {
    return pow(clamp(linearColor, 0.0, 1.0), vec3(1.0 / 2.2));
}

// Compute Phong terms for a single light contribution.
// Returns the summed ambient + diffuse + specular for the material.
vec3 phongContrib(
    vec3 normal,       // unit normal
    vec3 lightDir,     // unit vector from frag toward light
    vec3 viewDir,      // unit vector from frag toward camera
    vec3 La,           // light ambient colour
    vec3 Ld,           // light diffuse colour
    vec3 Ls            // light specular colour
) {
    // Ambient
    vec3 ambient  = La * uAmbientColor;

    // Diffuse — clamped Lambert
    float NdotL   = max(dot(normal, lightDir), 0.0);
    vec3 diffuse  = Ld * uDiffuseColor * NdotL;

    // Specular — Phong reflection
    vec3  reflDir = reflect(-lightDir, normal);
    float RdotV   = max(dot(reflDir, viewDir), 0.0);
    float spec    = (NdotL > 0.0) ? pow(RdotV, uShininess) : 0.0;
    vec3 specular = Ls * uSpecularColor * spec;

    return ambient + diffuse + specular;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
void main() {
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uViewPos - vFragPos);

    // --- Directional light --------------------------------------------------
    vec3 dirL = normalize(uDirLightDir);  // already points toward light
    vec3 colorDir = phongContrib(N, dirL, V,
                                 uDirLightAmbient,
                                 uDirLightDiffuse,
                                 uDirLightSpecular);

    // --- Point light --------------------------------------------------------
    vec3  toPoint    = uPointLightPos - vFragPos;
    float dist       = length(toPoint);
    vec3  pointL     = toPoint / dist;

    float attenuation = 1.0 / (uConstantAtten
                              + uLinearAtten    * dist
                              + uQuadraticAtten * dist * dist);

    vec3 colorPoint = phongContrib(N, pointL, V,
                                   uPointLightAmbient,
                                   uPointLightDiffuse,
                                   uPointLightSpecular) * attenuation;

    // --- Combine and gamma-correct ------------------------------------------
    vec3 linearResult = colorDir + colorPoint;
    FragColor = vec4(gammaCorrect(linearResult), 1.0);
}
