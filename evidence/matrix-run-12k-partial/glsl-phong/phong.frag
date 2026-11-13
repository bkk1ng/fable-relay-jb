#version 330 core

// ---------------------------------------------------------------------------
// Inputs from vertex shader
// ---------------------------------------------------------------------------
in vec3 vFragPos;   // world-space fragment position
in vec3 vNormal;    // world-space normal (may lose unit length after interpolation)

// ---------------------------------------------------------------------------
// Material
// ---------------------------------------------------------------------------
uniform vec3  uAmbientColor;        // material ambient reflectance
uniform vec3  uDiffuseColor;        // material diffuse reflectance
uniform vec3  uSpecularColor;       // material specular reflectance
uniform float uShininess;           // Phong specular exponent

// ---------------------------------------------------------------------------
// Directional light  (direction is FROM the light, toward the scene)
// ---------------------------------------------------------------------------
uniform vec3  uDirLightDir;         // world-space, need not be normalised here
uniform vec3  uDirLightAmbient;     // La
uniform vec3  uDirLightDiffuse;     // Ld
uniform vec3  uDirLightSpecular;    // Ls

// ---------------------------------------------------------------------------
// Point light
// ---------------------------------------------------------------------------
uniform vec3  uPointLightPos;       // world-space
uniform vec3  uPointLightAmbient;
uniform vec3  uPointLightDiffuse;
uniform vec3  uPointLightSpecular;
uniform float uConstantAtten;       // kc  — typically 1.0
uniform float uLinearAtten;         // kl
uniform float uQuadraticAtten;      // kq

// ---------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------
uniform vec3  uViewPos;             // world-space camera position

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------
out vec4 FragColor;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Blinn-Phong half-vector can be swapped in by replacing the reflect() call,
// but canonical Phong is kept here as requested.

vec3 phongDiffSpec(vec3 lightDir,  // unit vector FROM fragment TOWARD light
                   vec3 normal,    // unit outward normal
                   vec3 viewDir,   // unit vector FROM fragment TOWARD camera
                   vec3 Ld,
                   vec3 Ls)
{
    // Diffuse
    float diff    = max(dot(normal, lightDir), 0.0);
    vec3  diffuse = Ld * diff * uDiffuseColor;

    // Specular (Phong reflection model)
    vec3  reflectDir = reflect(-lightDir, normal);   // reflect expects incident (toward surface)
    float spec       = (diff > 0.0)
                       ? pow(max(dot(viewDir, reflectDir), 0.0), uShininess)
                       : 0.0;                        // no specular on the dark side
    vec3  specular   = Ls * spec * uSpecularColor;

    return diffuse + specular;
}

// ---------------------------------------------------------------------------
void main()
{
    vec3 normal  = normalize(vNormal);
    vec3 viewDir = normalize(uViewPos - vFragPos);

    // ---- Directional light -------------------------------------------------
    vec3 dirL = normalize(-uDirLightDir);            // toward light

    vec3 dirAmbient = uDirLightAmbient * uAmbientColor;
    vec3 dirContrib = dirAmbient
                    + phongDiffSpec(dirL, normal, viewDir,
                                    uDirLightDiffuse, uDirLightSpecular);

    // ---- Point light -------------------------------------------------------
    vec3  toPoint    = uPointLightPos - vFragPos;
    float dist       = length(toPoint);
    vec3  pointL     = toPoint / dist;               // unit toward light

    float attenuation = 1.0 / (uConstantAtten
                              + uLinearAtten    * dist
                              + uQuadraticAtten * dist * dist);

    vec3 pointAmbient = uPointLightAmbient * uAmbientColor;
    vec3 pointContrib = (pointAmbient
                        + phongDiffSpec(pointL, normal, viewDir,
                                        uPointLightDiffuse, uPointLightSpecular))
                        * attenuation;

    // ---- Combine -----------------------------------------------------------
    vec3 linearColor = dirContrib + pointContrib;

    // Gamma correction (linear -> sRGB, gamma = 2.2)
    vec3 gamma       = vec3(1.0 / 2.2);
    vec3 gammaCorrected = pow(clamp(linearColor, 0.0, 1.0), gamma);

    FragColor = vec4(gammaCorrected, 1.0);
}
