#pragma once

namespace nri_scene
{
struct PTTransform3x4
{
	float m[3][4] = {};
};

inline PTTransform3x4 MakeIdentityPTTransform3x4()
{
	PTTransform3x4 transform = {};
	transform.m[0][0] = 1.0f;
	transform.m[1][1] = 1.0f;
	transform.m[2][2] = 1.0f;
	return transform;
}

template<typename T>
inline void SetTopLevelInstanceTransform(T& instance, const PTTransform3x4& transform)
{
	for (int row = 0; row < 3; ++row)
	{
		for (int column = 0; column < 4; ++column)
		{
			instance.transform[row][column] = transform.m[row][column];
		}
	}
}
}
