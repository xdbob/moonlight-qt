#version 330 core
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
	
	//YCbCr -= vec3(0, vec2(0.5));
	YCbCr -= vec3(0.0627451017, 0.501960814, 0.501960814);
	FragColor = vec4(clamp(yuvmat * YCbCr, 0.0, 1.0), 1.0f);
}
