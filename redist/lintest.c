#include "linmath.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

bool assertFLTEquals(FLT a, FLT b) { return fabs(a - b) < 0.0001; }

int assertFLTAEquals(FLT *a, FLT *b, int length) {
	for (int i = 0; i < length; i++) {
		if (assertFLTEquals(a[i], b[i]) != true) {
			return i;
		}
	}
	return -1;
}

void printFLTA(FLT *a, int length) {
	for (int i = 0; i < length; i++)
		fprintf(stderr, "%.7f ", a[i]);
}

#define ASSERT_FLTA_EQUALS(a, b, l)                                                                                    \
	if (assertFLTAEquals(a, b, l) != -1) {                                                                             \
		fprintf(stderr, "Where '" #a "'= ");                                                                           \
		printFLTA(a, l);                                                                                               \
		fprintf(stderr, "\nWhere '" #b "'= ");                                                                         \
		printFLTA(b, l);                                                                                               \
		fprintf(stderr, "\n");                                                                                         \
		assert(assertFLTAEquals(a, b, l) == -1);                                                                       \
	}

void testInvertPose() {

	LinmathPose rotAroundYOffset = {1, 1, 1, .5, 0, .5, 0};
	LinmathPose pose_out;

	{
		LinmathPose expected = {1, -1, -1, 0.7071068, 0, -0.7071068, 0};
		InvertPose(&pose_out, &rotAroundYOffset);
		ASSERT_FLTA_EQUALS(pose_out.Pos, expected.Pos, 7);

		FLT identity[] = {0, 0, 0, 1, 0, 0, 0};
		ApplyPoseToPose(&pose_out, &expected, &rotAroundYOffset);
		quatnormalize(pose_out.Rot, pose_out.Rot);
		ASSERT_FLTA_EQUALS(pose_out.Pos, identity, 7);
	}
}

void testApplyPoseToPose() {
	LinmathPose rotAroundYOffset = {1, 1, 1, 0, 0, 1, 0};
	LinmathPose pose_out;

	{
		LinmathPose pt = {0, 1, 0, 0, 0, 1, 0};
		LinmathPose expected = {1, 2, 1, -1, 0, 0, 0};
		ApplyPoseToPose(&pose_out, &rotAroundYOffset, &pt);
		quatnormalize(pose_out.Rot, pose_out.Rot);
		ASSERT_FLTA_EQUALS(pose_out.Pos, expected.Pos, 7);
	}

	{
		LinmathPose pt = {0, 1, 0, 0, 1, 0, 0};
		LinmathPose expected = {1, 2, 1, 0, 0, 0, -1};
		ApplyPoseToPose(&pose_out, &rotAroundYOffset, &pt);
		ASSERT_FLTA_EQUALS(pose_out.Pos, expected.Pos, 7);
	}
}

void testApplyPoseToPoint() {
	LinmathPose rotAroundY = {0, 0, 0, 0, 0, 1, 0};
	FLT pt_out[3];

	{
		FLT pt[3] = {0, 1, 0};
		FLT expected[3] = {0, 1, 0};
		ApplyPoseToPoint(pt_out, &rotAroundY, pt);
		ASSERT_FLTA_EQUALS(pt_out, expected, 3);
	}

	{
		FLT pt[3] = {1, 1, 0};
		FLT expected[3] = {-1, 1, 0};
		ApplyPoseToPoint(pt_out, &rotAroundY, pt);
		ASSERT_FLTA_EQUALS(pt_out, expected, 3);
	}

	LinmathPose rotAroundYOffset = {1, 1, 1, 0, 0, 1, 0};

	{
		FLT pt[3] = {0, 1, 0};
		FLT expected[3] = {1, 2, 1};
		ApplyPoseToPoint(pt_out, &rotAroundYOffset, pt);
		ASSERT_FLTA_EQUALS(pt_out, expected, 3);
	}

	{
		FLT pt[3] = {1, 1, 0};
		FLT expected[3] = {0, 2, 1};
		ApplyPoseToPoint(pt_out, &rotAroundYOffset, pt);
		ASSERT_FLTA_EQUALS(pt_out, expected, 3);
	}
}

int main()
{
	testInvertPose();
	testApplyPoseToPoint();
	testApplyPoseToPose();
#if 1

#define NONTRANSPOSED_DAVE
#ifdef NONTRANSPOSED_DAVE
	FLT pLH1[3] = {-0.396888, 3.182945, -0.568622};
	FLT qLH1[4] = {0.668640, -0.576296, 0.103727, -0.458305};
	FLT pNLH1[3] = { 0.113572, 2.791495, -1.495652 };  //1M +x
	FLT qNLH1[4] = { 0.807419, 0.372818, -0.451339, 0.073308 };


	FLT pLH2[3] = {0.195579, 3.193770, -0.424473};
	FLT qLH2[4] = {0.401849, 0.104771, 0.580441, 0.700449};
	FLT pNLH2[3] = {-0.183505, 3.356293, 0.695688, };
	FLT qNLH2[4] = {-0.237438, 0.405213, 0.270438, 0.840410 };
#else

	FLT pLH1[3] = {-0.321299, 3.130532, -0.786460};
	FLT qLH1[4] = {0.794180, 0.336117, -0.485668, -0.142934};
	FLT pNLH1[3] = { 0.113572, 2.791495, -1.495652 };  //1M +x
	FLT qNLH1[4] = { 0.807419, 0.372818, -0.451339, 0.073308 };

	FLT pLH2[3] = {0.153580, 3.251673, -0.190491};
	FLT qLH2[4] = {0.217017, 0.482214, 0.306568, 0.791448 };
	FLT pNLH2[3] = {-0.175330, 3.351943, 0.669623 };
	FLT qNLH2[4] = {0.257241, 0.394159, 0.292555, 0.832392 };
#endif

	FLT pOut1[3];
	FLT pOut2[3];

	qLH1[0] *= -1;
	qLH2[0] *= -1;

	quatrotatevector( pOut1, qLH1, pLH1 );
	quatrotatevector( pOut2, qLH2, pLH2 );

	printf( "%f %f %f\n", PFTHREE( pOut1 ) );
	printf( "%f %f %f\n", PFTHREE( pOut2 ) );

//	qLH1[1]*=-1;
//	qLH2[0]*=-1;

/*
	sub3d( pOut1, pLH1, pNLH1 );
	sub3d( pOut2, pLH2, pNLH2 );


	printf( "%f %f %f\n", PFTHREE( pOut1 ) );
	printf( "%f %f %f\n", PFTHREE( pOut2 ) );

	quatrotatevector( pOut1, qLH1, pOut1 );
	quatrotatevector( pOut2, qLH2, pOut2 );

	printf( "%f %f %f\n", PFTHREE( pOut1 ) );
	printf( "%f %f %f\n", PFTHREE( pOut2 ) );
*/
	return 0;

#endif
	
}

