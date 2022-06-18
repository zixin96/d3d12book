#include <windows.h> // for XMVerifyCPUSupport
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <iostream>
using namespace std;
using namespace DirectX;
using namespace DirectX::PackedVector;

int main()
{
	cout.precision(8);

	// Check support for SSE2 (Pentium4, AMD K8, and above).
	if (!XMVerifyCPUSupport())
	{
		cout << "directx math not supported" << endl;
		return 0;
	}

	XMVECTOR u = XMVectorSet(1.0f, 1.0f, 1.0f, 0.0f);
	XMVECTOR n = XMVector3Normalize(u);

	float LU = XMVectorGetX(XMVector3Length(n));

	// Mathematically, the length should be 1.  Is it numerically?
	cout << LU << endl;
	if (LU == 1.0f)
		cout << "Length 1" << endl;
	else
		cout << "Length not 1" << endl;

	// Raising 1 to any power should still be 1.  Is it?
	float powLU = powf(LU, 1.0e6f);
	cout << "LU^(10^6) = " << powLU << endl;

	//! Use DirectX built-in function to compare floating point values
	cout << "Use DirectX XMScalarNearEqual to compare floating point values:" << endl;
	if (XMScalarNearEqual(powLU, 1.0f, 0.1f))
		cout << "Length 1" << endl;
	else
		cout << "Length not 1" << endl;

	cout << "Use DirectX XMVector3NearEqual to compare vector values:" << endl;
	XMVECTOR lengthTest = XMVectorSet(1.0f, 1.0f, 1.0f, 1.0f);
	XMVECTOR lengthTest2 = XMVectorReplicate(powLU);
	XMVECTOR EPLIV = XMVectorReplicate(0.00001f);
	if (XMVector3NearEqual(lengthTest, lengthTest2, EPLIV))
		cout << "Length 1" << endl;
	else
		cout << "Length not 1" << endl;
}
