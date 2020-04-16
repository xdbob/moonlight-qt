#version 300 es
#extension GL_OES_EGL_image_external : require
precision mediump float;
out vec4 FragColor;

in vec2 vTextCoord;

uniform mat3 yuvmat;
uniform samplerExternalOES plane1;
uniform samplerExternalOES plane2;

void main() {
	vec3 YCbCr = vec3(
		texture2D(plane1, vTextCoord)[0],
		texture2D(plane2, vTextCoord).xy
	);

	// TODO: this should be an offset parameter
	YCbCr -= vec3(0.0627451017, 0.501960814, 0.501960814);
	FragColor = vec4(clamp(yuvmat * YCbCr, 0.0, 1.0), 1.0f);
}
