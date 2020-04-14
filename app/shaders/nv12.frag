#version 330 core
out vec4 FragColor;

in vec2 vTextCoord;

uniform mat4 yuvmat;
uniform sampler2D plane1;
uniform sampler2D plane2;

vec4 tmp;

void main() {
	//FragColor = vec4(1.0f, 0.5f, 0.2f, 1.0f);
	vec3 YCbr = vec3(
		texture(plane1, vTextCoord)[0],
		texture(plane2, vTextCoord).xy
	);
	float grey = YCbr[0];
	FragColor = vec4(grey, grey, grey, 1.0f);
}
