/*
	This file is part of the implementation for the technical paper

		Field-Aligned Online Surface Reconstruction
		Nico Schertler, Marco Tarini, Wenzel Jakob, Misha Kazhdan, Stefan Gumhold, Daniele Panozzo
		ACM TOG 36, 4, July 2017 (Proceedings of SIGGRAPH 2017)

	Use of this source code is granted via a BSD-style license, which can be found
	in License.txt in the repository root.

	@author Nico Schertler
*/

#include "osr/Scan.h"

#include "osr/gui/ShaderPool.h"
#include "osr/Colors.h"

#include "osr/HierarchyDef.h"

#include <tbb/tbb.h>

#include <fstream>

using namespace osr;

Scan::Scan(const Matrix3Xf& V, const Matrix3Xf& N, const Matrix3Xus& C, const MatrixXu& F, const std::string& name, const Eigen::Affine3f& transform)
	: positionBuffer(nse::gui::VertexBuffer), normalBuffer(nse::gui::VertexBuffer), colorBuffer(nse::gui::VertexBuffer), indexBuffer(nse::gui::IndexBuffer),
	showInput(true), showNormals(false), mTransform(transform)
{
	mV = V;
	mN = N;
	mC = C;
	mF = F;
	this->name = name;

	bbox.reset();
	bbox.expand(mV);

	for (int i = 0; i < mN.cols(); ++i)
		mN.col(i).normalize();	
}

Scan::~Scan()
{
	if (kdTree)
		delete kdTree;
}

void Scan::initialize()
{	
	calculateNormals();		

	if (false)
	{
		std::mt19937 eng;
		std::normal_distribution<float> noise(0.0f, 0.15f);
		for (int v = 0; v < mN.cols(); ++v)
		{
			mN.col(v) += Vector3f(noise(eng), noise(eng), noise(eng));

			mN.col(v).normalize();			
		}
	}

	inputMesh.generate();

	Matrix3Xf dummy(3, 1);
	gui::ShaderPool::Instance()->ObjectShader.bind();
	inputMesh.bind();
	positionBuffer.uploadData(dummy).bindToAttribute("position");
	normalBuffer.uploadData(dummy).bindToAttribute("normal");
	colorBuffer.uploadData(dummy).bindToAttribute("color");	
	indexBuffer.uploadData(mF);

	gui::ShaderPool::Instance()->NormalShader.bind();
	positionBuffer.bindToAttribute("position");
	normalBuffer.bindToAttribute("normal");
	inputMesh.unbind();

	uploadData();

	indexCount = mF.rows() * mF.cols();
}

void Scan::calculateNormalsFromFaces()
{
	//Area-weighted average of incident face normals
	mN.resize(3, mV.cols());
	mN.setConstant(0);
	for (int f = 0; f < mF.cols(); ++f)
	{
		Vector3f v0 = mV.col(mF(0, f)),
			v1 = mV.col(mF(1, f)),
			v2 = mV.col(mF(2, f)),
			n = (v1 - v0).cross(v2 - v0);

		mN.col(mF(0, f)) += n;
		mN.col(mF(1, f)) += n;
		mN.col(mF(2, f)) += n;
	}

	for (int v = 0; v < mN.cols(); ++v)
	{
		mN.col(v).normalize();
	}
}

void Scan::calculateNormalsPCA()
{
	nse::util::TimedBlock b("Calculating normals with PCA ..");
	
	bool hasTree = kdTree == nullptr;
	buildTree();

	//Simple PCA
	mN.resize(3, mV.cols());
#pragma omp parallel for
	for(int i = 0; i < mV.cols(); ++i)
	{
		const int k = 20;
		Eigen::Vector3f centroid(0, 0, 0);

		std::vector<size_t> neighbors(k);
		std::vector<float> distances(k);
		Eigen::Vector3f point = mV.col(i);
		kdTree->knnSearch(point.data(), k, neighbors.data(), distances.data());
		
		for (auto p : neighbors)
			centroid += V().col(p);

		centroid *= (1.0f / neighbors.size());

		Matrix3f covariance; covariance.setConstant(0.0f);
		for (auto p : neighbors)
		{
			auto d = V().col(p) - centroid;
			covariance += d * d.transpose();
		}

		Eigen::JacobiSVD<MatrixXf> svd(covariance, Eigen::ComputeFullU);
		mN.col(i) = svd.matrixU().col(2);

		//assume that the scanner is located at the local origin
		if (mN.col(i).dot(mV.col(i)) > 0)
			mN.col(i) *= -1;
	}	

	if (!hasTree)
	{
		delete kdTree;
		kdTree = nullptr;
	}
}

void Scan::draw(const Eigen::Matrix4f & v, const Eigen::Matrix4f & proj) const
{
	if (!showInput && !showNormals)
		return;

	Eigen::Matrix4f mv = v * mTransform.matrix();
	Eigen::Matrix4f mvp = proj * mv;

	if (showInput)
	{
		glPointSize(5);
		//Draw input Mesh
		auto& shader = gui::ShaderPool::Instance()->ObjectShader;
		shader.bind();
		shader.setUniform("mv", mv);
		shader.setUniform("mvp", mvp);
		inputMesh.bind();
		if(mF.size() == 0)
			glDrawArrays(GL_POINTS, 0, V().cols());
		else
			glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, 0);
		inputMesh.unbind();
	}

	if (showNormals)
	{
		auto& shader = gui::ShaderPool::Instance()->NormalShader;
		shader.bind();
		shader.setUniform("mvp", mvp);
		shader.setUniform("scale", 0.01f * bbox.diagonal().norm());
		inputMesh.bind();
		glDrawArrays(GL_POINTS, 0, V().cols());
		inputMesh.unbind();
	}
}

void Scan::calculateNormals()
{
	if (mN.size() == 0)
	{
		if (mF.size() > 0)
			calculateNormalsFromFaces();
		else
			calculateNormalsPCA();
	}
}

Vector3f Scan::p(size_t idx) const
{
	return mTransform * mV.col(idx);
}

Vector3f Scan::n(size_t idx) const
{
	return mTransform.linear() * mN.col(idx);
}

nse::math::BoundingBox<float, 3> Scan::getTransformedBoundingBox() const
{
	return bbox.transform(mTransform);
}

void Scan::uploadData()
{
	if (mV.size() > 0)
	{
		bbox.expand(mV);
		positionBuffer.uploadData(mV);
	}

	if (mN.size() > 0)
		normalBuffer.uploadData(mN);

	if (mC.size() > 0)
	{
		Matrix3Xf C_gpu(3, mC.cols());
#pragma omp parallel for
		for (int i = 0; i < mC.cols(); ++i)
		{
			C_gpu.col(i) = LabToRGB(mC.col(i)).cast<float>() / 65535.0f;
		}
		colorBuffer.uploadData(C_gpu);
	}
}

void Scan::cleanOverlap(const THierarchy& hierarchy, float distance)
{
#pragma omp parallel for
	for (int i = 0; i < mV.cols(); ++i)
	{
		bool isOverlapping = false;
		hierarchy.findNearestPointsRadius(mTransform * mV.col(i), distance, [&](const auto&, auto) { isOverlapping = true; });

		if (isOverlapping)
			mV.col(i).setConstant(std::numeric_limits<float>::quiet_NaN());
	}

	uploadData();
}

struct ClosestCompatibleAggregator
{
	float radius;
	const Scan& scan;

	size_t result = -1;

	const Vector3f referenceNormal;

	ClosestCompatibleAggregator(float radius, const Scan& scan, const Vector3f& referenceNormal)
		: radius(radius), scan(scan), referenceNormal(referenceNormal)
	{ }

	void init() { }
	void clear() { result = -1; }
	size_t size() { return result == -1 ? 0 : 1; }
	bool full() const { return false; }
	float worstDist() const 
	{
		return radius; 
	}

	void addPoint(float distance, uint32_t index)
	{
		auto& n = scan.N().col(index);
		if (n.dot(referenceNormal) < 0.7f)
			return;

		if (distance < radius)
		{
			radius = distance;
			result = index;
		}
	}
};

size_t Scan::findClosestCompatiblePoint(const Vector3f & p, const Vector3f & n) const
{
	const float radius = closestPointRadius;

	if (kdTree == nullptr)
		throw std::runtime_error("Must create the kd tree before being able to query points.");

	nanoflann::SearchParams params(0, 0, false);
	ClosestCompatibleAggregator result(radius * radius, *this, n);

	kdTree->radiusSearchCustomCallback(p.data(), result, params);

	return result.result;
}