#version 300 es
precision mediump float;
out vec4 FragColor;

in vec2 vTextCoord;

uniform mat3 yuvmat;
uniform sampler2D plane1;
uniform sampler2D plane2;

void main() {
	vec3 YCbCr = vec3(
		texture(plane1, vTextCoord)[0],
		texture(plane2, vTextCoord).xy
	);

	// TODO: this should be an offset parameter
	YCbCr -= vec3(0.0627451017, 0.501960814, 0.501960814);
	FragColor = vec4(clamp(yuvmat * YCbCr, 0.0, 1.0), 1.0f);
}
