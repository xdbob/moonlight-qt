#version 330 core

layout (location = 0) in vec2 aPosition; // 2D: X,Y
layout (location = 1) in vec2 aTexCoord;
out vec2 vTextCoord;
// univorm vec4 uMatrix;

void main() {
	vTextCoord = aTexCoord;
	//gl_Position = uMatrix * aPosition;
	gl_Position = vec4(aPosition, 0, 1);
}
