#pragma once

#include <Eigen/Core>
#include <Eigen/QR>

#include "com/CommunicateMesh.hpp"
#include "com/Communication.hpp"
#include "impl/BasisFunctions.hpp"
#include "mapping/Mapping.hpp"
#include "mesh/Filter.hpp"
#include "precice/types.hpp"
#include "utils/EigenHelperFunctions.hpp"
#include "utils/Event.hpp"
#include "utils/IntraComm.hpp"

namespace precice {
extern bool syncMode;

namespace mapping {

/**
 * @brief Mapping with radial basis functions.
 *
 * With help of the input data points and values an interpolant is constructed.
 * The interpolant is formed by a weighted sum of conditionally positive radial
 * basis functions and a (low order) polynomial, and evaluated at the output
 * data points.
 *
 * The radial basis function type has to be given as template parameter, and has
 * to be one of the defined types in this file.
 */
template <typename RADIAL_BASIS_FUNCTION_T>
class RadialBasisFctMapping : public Mapping {
public:
  /**
   * @brief Constructor.
   *
   * @param[in] constraint Specifies mapping to be consistent or conservative.
   * @param[in] dimensions Dimensionality of the meshes
   * @param[in] function Radial basis function used for mapping.
   * @param[in] xDead, yDead, zDead Deactivates mapping along an axis
   */
  RadialBasisFctMapping(
      Constraint              constraint,
      int                     dimensions,
      RADIAL_BASIS_FUNCTION_T function,
      bool                    xDead,
      bool                    yDead,
      bool                    zDead);

  /// Computes the mapping coefficients from the in- and output mesh.
  virtual void computeMapping() override;

  /// Returns true, if computeMapping() has been called.
  virtual bool hasComputedMapping() const override;

  /// Removes a computed mapping.
  virtual void clear() override;

  /// Maps input data to output data from input mesh to output mesh.
  virtual void map(int inputDataID, int outputDataID) override;

  virtual void tagMeshFirstRound() override;

  virtual void tagMeshSecondRound() override;

private:
  precice::logging::Logger _log{"mapping::RadialBasisFctMapping"};

  bool _hasComputedMapping = false;

  /// Radial basis function type used in interpolation.
  RADIAL_BASIS_FUNCTION_T _basisFunction;

  Eigen::MatrixXd _matrixA;

  Eigen::ColPivHouseholderQR<Eigen::MatrixXd> _qr;

  /// true if the mapping along some axis should be ignored
  std::vector<bool> _deadAxis;

  void mapConservative(int inputDataID, int outputDataID, int polyparams);
  void mapConsistent(int inputDataID, int outputDataID, int polyparams);

  void setDeadAxis(bool xDead, bool yDead, bool zDead)
  {
    _deadAxis.resize(getDimensions());
    if (getDimensions() == 2) {
      _deadAxis[0] = xDead;
      _deadAxis[1] = yDead;
      PRECICE_CHECK(not(xDead && yDead), "You cannot choose all axes to be dead for a RBF mapping");
      if (zDead)
        PRECICE_WARN("Setting the z-axis to dead on a 2-dimensional problem has no effect.");
    } else if (getDimensions() == 3) {
      _deadAxis[0] = xDead;
      _deadAxis[1] = yDead;
      _deadAxis[2] = zDead;
      PRECICE_CHECK(not(xDead && yDead && zDead), "You cannot choose all axes to be dead for a RBF mapping");
    } else {
      PRECICE_ASSERT(false);
    }
  }
};

// --------------------------------------------------- HEADER IMPLEMENTATIONS

template <typename RADIAL_BASIS_FUNCTION_T>
RadialBasisFctMapping<RADIAL_BASIS_FUNCTION_T>::RadialBasisFctMapping(
    Constraint              constraint,
    int                     dimensions,
    RADIAL_BASIS_FUNCTION_T function,
    bool                    xDead,
    bool                    yDead,
    bool                    zDead)
    : Mapping(constraint, dimensions),
      _basisFunction(function)
{
  if (constraint == SCALEDCONSISTENT) {
    setInputRequirement(Mapping::MeshRequirement::FULL);
    setOutputRequirement(Mapping::MeshRequirement::FULL);
  } else {
    setInputRequirement(Mapping::MeshRequirement::VERTEX);
    setOutputRequirement(Mapping::MeshRequirement::VERTEX);
  }
  setDeadAxis(xDead, yDead, zDead);
}

template <typename RADIAL_BASIS_FUNCTION_T>
void RadialBasisFctMapping<RADIAL_BASIS_FUNCTION_T>::computeMapping()
{
  PRECICE_TRACE();

  precice::utils::Event e("map.rbf.computeMapping.From" + input()->getName() + "To" + output()->getName(), precice::syncMode);

  PRECICE_ASSERT(input()->getDimensions() == output()->getDimensions(),
                 input()->getDimensions(), output()->getDimensions());
  PRECICE_ASSERT(getDimensions() == output()->getDimensions(),
                 getDimensions(), output()->getDimensions());

  mesh::PtrMesh inMesh;
  mesh::PtrMesh outMesh;

  if (hasConstraint(CONSERVATIVE)) {
    inMesh  = output();
    outMesh = input();
  } else { // Consistent or scaled consistent
    inMesh  = input();
    outMesh = output();
  }

  if (utils::IntraComm::isSecondary()) {

    // Input mesh may have overlaps
    mesh::Mesh filteredInMesh("filteredInMesh", inMesh->getDimensions(), mesh::Mesh::MESH_ID_UNDEFINED);
    mesh::filterMesh(filteredInMesh, *inMesh, [&](const mesh::Vertex &v) { return v.isOwner(); });

    // Send the mesh
    com::CommunicateMesh(utils::IntraComm::getCommunication()).sendMesh(filteredInMesh, 0);
    com::CommunicateMesh(utils::IntraComm::getCommunication()).sendMesh(*outMesh, 0);

  } else { // Parallel Primary rank or Serial

    mesh::Mesh globalInMesh("globalInMesh", inMesh->getDimensions(), mesh::Mesh::MESH_ID_UNDEFINED);
    mesh::Mesh globalOutMesh("globalOutMesh", outMesh->getDimensions(), mesh::Mesh::MESH_ID_UNDEFINED);

    if (utils::IntraComm::isPrimary()) {
      {
        // Input mesh may have overlaps
        mesh::Mesh filteredInMesh("filteredInMesh", inMesh->getDimensions(), mesh::Mesh::MESH_ID_UNDEFINED);
        mesh::filterMesh(filteredInMesh, *inMesh, [&](const mesh::Vertex &v) { return v.isOwner(); });
        globalInMesh.addMesh(filteredInMesh);
        globalOutMesh.addMesh(*outMesh);
      }

      // Receive mesh
      for (Rank secondaryRank : utils::IntraComm::allSecondaryRanks()) {
        mesh::Mesh secondaryInMesh(inMesh->getName(), inMesh->getDimensions(), mesh::Mesh::MESH_ID_UNDEFINED);
        com::CommunicateMesh(utils::IntraComm::getCommunication()).receiveMesh(secondaryInMesh, secondaryRank);
        globalInMesh.addMesh(secondaryInMesh);

        mesh::Mesh secondaryOutMesh(outMesh->getName(), outMesh->getDimensions(), mesh::Mesh::MESH_ID_UNDEFINED);
        com::CommunicateMesh(utils::IntraComm::getCommunication()).receiveMesh(secondaryOutMesh, secondaryRank);
        globalOutMesh.addMesh(secondaryOutMesh);
      }

    } else { // Serial
      globalInMesh.addMesh(*inMesh);
      globalOutMesh.addMesh(*outMesh);
    }

    _matrixA = buildMatrixA(_basisFunction, globalInMesh, globalOutMesh, _deadAxis);
    _qr      = buildMatrixCLU(_basisFunction, globalInMesh, _deadAxis).colPivHouseholderQr();

    PRECICE_CHECK(_qr.isInvertible(),
                  "The interpolation matrix of the RBF mapping from mesh {} to mesh {} is not invertable. "
                  "This means that the mapping problem is not well-posed. "
                  "Please check if your coupling meshes are correct. Maybe you need to fix axis-aligned mapping setups "
                  "by marking perpendicular axes as dead?",
                  input()->getName(), output()->getName());
  }
  _hasComputedMapping = true;
  PRECICE_DEBUG("Compute Mapping is Completed.");
} // namespace mapping

template <typename RADIAL_BASIS_FUNCTION_T>
bool RadialBasisFctMapping<RADIAL_BASIS_FUNCTION_T>::hasComputedMapping() const
{
  return _hasComputedMapping;
}

template <typename RADIAL_BASIS_FUNCTION_T>
void RadialBasisFctMapping<RADIAL_BASIS_FUNCTION_T>::clear()
{
  PRECICE_TRACE();
  _matrixA            = Eigen::MatrixXd();
  _qr                 = Eigen::ColPivHouseholderQR<Eigen::MatrixXd>();
  _hasComputedMapping = false;
}

template <typename RADIAL_BASIS_FUNCTION_T>
void RadialBasisFctMapping<RADIAL_BASIS_FUNCTION_T>::map(
    int inputDataID,
    int outputDataID)
{
  PRECICE_TRACE(inputDataID, outputDataID);

  precice::utils::Event e("map.rbf.mapData.From" + input()->getName() + "To" + output()->getName(), precice::syncMode);

  PRECICE_ASSERT(_hasComputedMapping);
  PRECICE_ASSERT(input()->getDimensions() == output()->getDimensions(),
                 input()->getDimensions(), output()->getDimensions());
  PRECICE_ASSERT(getDimensions() == output()->getDimensions(),
                 getDimensions(), output()->getDimensions());
  {
    int valueDim = input()->data(inputDataID)->getDimensions();
    PRECICE_ASSERT(valueDim == output()->data(outputDataID)->getDimensions(),
                   valueDim, output()->data(outputDataID)->getDimensions());
  }
  int deadDimensions = 0;
  for (int d = 0; d < getDimensions(); d++) {
    if (_deadAxis[d])
      deadDimensions += 1;
  }
  int polyparams = 1 + getDimensions() - deadDimensions;

  if (hasConstraint(CONSERVATIVE)) {
    mapConservative(inputDataID, outputDataID, polyparams);
  } else {
    mapConsistent(inputDataID, outputDataID, polyparams);
  }
}

template <typename RADIAL_BASIS_FUNCTION_T>
void RadialBasisFctMapping<RADIAL_BASIS_FUNCTION_T>::mapConservative(int inputDataID, int outputDataID, int polyparams)
{

  PRECICE_TRACE(inputDataID, outputDataID, polyparams);

  // Gather input data
  if (utils::IntraComm::isSecondary()) {

    const auto &localInData = input()->data(inputDataID)->values();

    int localOutputSize = 0;
    for (const auto &vertex : output()->vertices()) {
      if (vertex.isOwner()) {
        ++localOutputSize;
      }
    }

    localOutputSize *= output()->data(outputDataID)->getDimensions();

    utils::IntraComm::getCommunication()->send(localInData, 0);
    utils::IntraComm::getCommunication()->send(localOutputSize, 0);

  } else { // Parallel Primary rank or Serial case

    std::vector<double> globalInValues;
    std::vector<double> outputValueSizes;
    {
      const auto &localInData = input()->data(inputDataID)->values();
      globalInValues.insert(globalInValues.begin(), localInData.data(), localInData.data() + localInData.size());

      int localOutputSize = 0;
      for (const auto &vertex : output()->vertices()) {
        if (vertex.isOwner()) {
          ++localOutputSize;
        }
      }

      localOutputSize *= output()->data(outputDataID)->getDimensions();

      outputValueSizes.push_back(localOutputSize);
    }

    {
      std::vector<double> secondaryBuffer;
      int                 secondaryOutputValueSize;
      for (Rank rank : utils::IntraComm::allSecondaryRanks()) {
        utils::IntraComm::getCommunication()->receive(secondaryBuffer, rank);
        globalInValues.insert(globalInValues.end(), secondaryBuffer.begin(), secondaryBuffer.end());

        utils::IntraComm::getCommunication()->receive(secondaryOutputValueSize, rank);
        outputValueSizes.push_back(secondaryOutputValueSize);
      }
    }

    int valueDim = output()->data(outputDataID)->getDimensions();

    // Construct Eigen vectors
    Eigen::Map<Eigen::VectorXd> inputValues(globalInValues.data(), globalInValues.size());
    Eigen::VectorXd             outputValues((_matrixA.cols() - polyparams) * valueDim);
    outputValues.setZero();

    Eigen::VectorXd Au(_matrixA.cols());  // rows == n
    Eigen::VectorXd in(_matrixA.rows());  // rows == outputSize
    Eigen::VectorXd out(_matrixA.cols()); // rows == n

    for (int dim = 0; dim < valueDim; dim++) {
      for (int i = 0; i < in.size(); i++) { // Fill input data values
        in[i] = inputValues(i * valueDim + dim);
      }

      Au  = _matrixA.transpose() * in;
      out = _qr.solve(Au);

      // Copy mapped data to output data values
      for (int i = 0; i < out.size() - polyparams; i++) {
        outputValues[i * valueDim + dim] = out[i];
      }
    }

    // Data scattering to secondary ranks
    if (utils::IntraComm::isPrimary()) {

      // Filter data
      int outputCounter = 0;
      for (int i = 0; i < static_cast<int>(output()->vertices().size()); ++i) {
        if (output()->vertices()[i].isOwner()) {
          for (int dim = 0; dim < valueDim; ++dim) {
            output()->data(outputDataID)->values()[i * valueDim + dim] = outputValues(outputCounter);
            ++outputCounter;
          }
        }
      }

      // Data scattering to secondary ranks
      int beginPoint = outputValueSizes.at(0);
      for (Rank rank : utils::IntraComm::allSecondaryRanks()) {
        precice::span<const double> toSend{outputValues.data() + beginPoint, static_cast<size_t>(outputValueSizes.at(rank))};
        utils::IntraComm::getCommunication()->send(toSend, rank);
        beginPoint += outputValueSizes.at(rank);
      }
    } else { // Serial
      output()->data(outputDataID)->values() = outputValues;
    }
  }
  if (utils::IntraComm::isSecondary()) {
    std::vector<double> receivedValues;
    utils::IntraComm::getCommunication()->receive(receivedValues, 0);

    int valueDim = output()->data(outputDataID)->getDimensions();

    int outputCounter = 0;
    for (int i = 0; i < static_cast<int>(output()->vertices().size()); ++i) {
      if (output()->vertices()[i].isOwner()) {
        for (int dim = 0; dim < valueDim; ++dim) {
          output()->data(outputDataID)->values()[i * valueDim + dim] = receivedValues.at(outputCounter);
          ++outputCounter;
        }
      }
    }
  }
}

template <typename RADIAL_BASIS_FUNCTION_T>
void RadialBasisFctMapping<RADIAL_BASIS_FUNCTION_T>::mapConsistent(int inputDataID, int outputDataID, int polyparams)
{

  PRECICE_TRACE(inputDataID, outputDataID, polyparams);

  // Gather input data
  if (utils::IntraComm::isSecondary()) {
    // Input data is filtered
    auto localInDataFiltered = input()->getOwnedVertexData(inputDataID);
    int  localOutputSize     = output()->data(outputDataID)->values().size();

    // Send data and output size
    utils::IntraComm::getCommunication()->send(localInDataFiltered, 0);
    utils::IntraComm::getCommunication()->send(localOutputSize, 0);

  } else { // Primary rank or Serial case

    int valueDim = output()->data(outputDataID)->getDimensions();

    std::vector<double> globalInValues((_matrixA.cols() - polyparams) * valueDim, 0.0);
    std::vector<int>    outValuesSize;

    if (utils::IntraComm::isPrimary()) { // Parallel case

      // Filter input data
      const auto &localInData = input()->getOwnedVertexData(inputDataID);
      std::copy(localInData.data(), localInData.data() + localInData.size(), globalInValues.begin());
      outValuesSize.push_back(output()->data(outputDataID)->values().size());

      int inputSizeCounter = localInData.size();
      int secondaryOutDataSize{0};

      std::vector<double> secondaryBuffer;

      for (Rank rank : utils::IntraComm::allSecondaryRanks()) {
        utils::IntraComm::getCommunication()->receive(secondaryBuffer, rank);
        std::copy(secondaryBuffer.begin(), secondaryBuffer.end(), globalInValues.begin() + inputSizeCounter);
        inputSizeCounter += secondaryBuffer.size();

        utils::IntraComm::getCommunication()->receive(secondaryOutDataSize, rank);
        outValuesSize.push_back(secondaryOutDataSize);
      }

    } else { // Serial case
      const auto &localInData = input()->data(inputDataID)->values();
      std::copy(localInData.data(), localInData.data() + localInData.size(), globalInValues.begin());
      outValuesSize.push_back(output()->data(outputDataID)->values().size());
    }

    Eigen::VectorXd p(_matrixA.cols());   // rows == n
    Eigen::VectorXd in(_matrixA.cols());  // rows == n
    Eigen::VectorXd out(_matrixA.rows()); // rows == outputSize
    in.setZero();

    // Construct Eigen vectors
    Eigen::Map<Eigen::VectorXd> inputValues(globalInValues.data(), globalInValues.size());

    Eigen::VectorXd outputValues((_matrixA.rows()) * valueDim);
    outputValues.setZero();

    // For every data dimension, perform mapping
    for (int dim = 0; dim < valueDim; dim++) {
      // Fill input from input data values (last polyparams entries remain zero)
      for (int i = 0; i < in.size() - polyparams; i++) {
        in[i] = inputValues[i * valueDim + dim];
      }

      p   = _qr.solve(in);
      out = _matrixA * p;

      // Copy mapped data to output data values
      for (int i = 0; i < out.size(); i++) {
        outputValues[i * valueDim + dim] = out[i];
      }
    }

    output()->data(outputDataID)->values() = Eigen::Map<Eigen::VectorXd>(outputValues.data(), outValuesSize.at(0));

    // Data scattering to secondary ranks
    int beginPoint = outValuesSize.at(0);

    if (utils::IntraComm::isPrimary()) {
      for (Rank rank : utils::IntraComm::allSecondaryRanks()) {
        precice::span<const double> toSend{outputValues.data() + beginPoint, static_cast<size_t>(outValuesSize.at(rank))};
        utils::IntraComm::getCommunication()->send(toSend, rank);
        beginPoint += outValuesSize.at(rank);
      }
    }
  }
  if (utils::IntraComm::isSecondary()) {
    std::vector<double> receivedValues;
    utils::IntraComm::getCommunication()->receive(receivedValues, 0);
    output()->data(outputDataID)->values() = Eigen::Map<Eigen::VectorXd>(receivedValues.data(), receivedValues.size());
  }
  if (hasConstraint(SCALEDCONSISTENT)) {
    scaleConsistentMapping(inputDataID, outputDataID);
  }
}

template <typename RADIAL_BASIS_FUNCTION_T>
void RadialBasisFctMapping<RADIAL_BASIS_FUNCTION_T>::tagMeshFirstRound()
{
  PRECICE_TRACE();
  mesh::PtrMesh filterMesh, otherMesh;
  if (hasConstraint(CONSERVATIVE)) {
    filterMesh = output(); // remote
    otherMesh  = input();  // local
  } else {
    filterMesh = input();  // remote
    otherMesh  = output(); // local
  }

  if (otherMesh->vertices().empty())
    return; // Ranks not at the interface should never hold interface vertices

  // Tags all vertices that are inside otherMesh's bounding box, enlarged by the support radius

  if (_basisFunction.hasCompactSupport()) {
    auto bb = otherMesh->getBoundingBox();
    bb.expandBy(_basisFunction.getSupportRadius());

    auto vertices = filterMesh->index().getVerticesInsideBox(bb);
    std::for_each(vertices.begin(), vertices.end(), [&filterMesh](size_t v) { filterMesh->vertices()[v].tag(); });
  } else {
    filterMesh->tagAll();
  }
}

template <typename RADIAL_BASIS_FUNCTION_T>
void RadialBasisFctMapping<RADIAL_BASIS_FUNCTION_T>::tagMeshSecondRound()
{
  PRECICE_TRACE();

  if (not _basisFunction.hasCompactSupport())
    return; // Tags should not be changed

  mesh::PtrMesh mesh; // The mesh we want to filter

  if (hasConstraint(CONSERVATIVE)) {
    mesh = output();
  } else {
    mesh = input();
  }

  mesh::BoundingBox bb(mesh->getDimensions());

  // Construct bounding box around all owned vertices
  for (mesh::Vertex &v : mesh->vertices()) {
    if (v.isOwner()) {
      PRECICE_ASSERT(v.isTagged()); // Should be tagged from the first round
      bb.expandBy(v);
    }
  }
  // Enlarge bb by support radius
  bb.expandBy(_basisFunction.getSupportRadius());
  auto vertices = mesh->index().getVerticesInsideBox(bb);
  std::for_each(vertices.begin(), vertices.end(), [&mesh](size_t v) { mesh->vertices()[v].tag(); });
}

// ------- Non-Member Functions ---------

template <typename RADIAL_BASIS_FUNCTION_T>
Eigen::MatrixXd buildMatrixCLU(RADIAL_BASIS_FUNCTION_T basisFunction, const mesh::Mesh &inputMesh, std::vector<bool> deadAxis)
{
  int inputSize  = inputMesh.vertices().size();
  int dimensions = inputMesh.getDimensions();

  int deadDimensions = 0;
  for (int d = 0; d < dimensions; d++) {
    if (deadAxis[d])
      deadDimensions += 1;
  }

  int polyparams = 1 + dimensions - deadDimensions;
  PRECICE_ASSERT(inputSize >= 1 + polyparams, inputSize);
  int n = inputSize + polyparams; // Add linear polynom degrees

  Eigen::MatrixXd matrixCLU(n, n);
  matrixCLU.setZero();

  for (int i = 0; i < inputSize; ++i) {
    for (int j = i; j < inputSize; ++j) {
      const auto &u   = inputMesh.vertices()[i].getCoords();
      const auto &v   = inputMesh.vertices()[j].getCoords();
      matrixCLU(i, j) = basisFunction.evaluate(utils::reduceVector((u - v), deadAxis).norm());
    }

    const auto reduced = utils::reduceVector(inputMesh.vertices()[i].getCoords(), deadAxis);

    for (int dim = 0; dim < dimensions - deadDimensions; dim++) {
      matrixCLU(i, inputSize + 1 + dim) = reduced[dim];
    }
    matrixCLU(i, inputSize) = 1.0;
  }

  matrixCLU.triangularView<Eigen::Lower>() = matrixCLU.transpose();

  return matrixCLU;
}

template <typename RADIAL_BASIS_FUNCTION_T>
Eigen::MatrixXd buildMatrixA(RADIAL_BASIS_FUNCTION_T basisFunction, const mesh::Mesh &inputMesh, const mesh::Mesh &outputMesh, std::vector<bool> deadAxis)
{
  int inputSize  = inputMesh.vertices().size();
  int outputSize = outputMesh.vertices().size();
  int dimensions = inputMesh.getDimensions();

  int deadDimensions = 0;
  for (int d = 0; d < dimensions; d++) {
    if (deadAxis[d])
      deadDimensions += 1;
  }

  int polyparams = 1 + dimensions - deadDimensions;
  PRECICE_ASSERT(inputSize >= 1 + polyparams, inputSize);
  int n = inputSize + polyparams; // Add linear polynom degrees

  Eigen::MatrixXd matrixA(outputSize, n);
  matrixA.setZero();

  // Fill _matrixA with values
  for (int i = 0; i < outputSize; ++i) {
    for (int j = 0; j < inputSize; ++j) {
      const auto &u = outputMesh.vertices()[i].getCoords();
      const auto &v = inputMesh.vertices()[j].getCoords();
      matrixA(i, j) = basisFunction.evaluate(utils::reduceVector((u - v), deadAxis).norm());
    }

    const auto reduced = utils::reduceVector(outputMesh.vertices()[i].getCoords(), deadAxis);

    for (int dim = 0; dim < dimensions - deadDimensions; dim++) {
      matrixA(i, inputSize + 1 + dim) = reduced[dim];
    }
    matrixA(i, inputSize) = 1.0;
  }
  return matrixA;
}

} // namespace mapping
} // namespace precice
