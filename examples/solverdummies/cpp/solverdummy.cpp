#include <iostream>
#include <sstream>
#include "precice/SolverInterface.hpp"

int main(int argc, char **argv)
{
  int commRank = 0;
  int commSize = 1;

  using namespace precice;
  using namespace precice::constants;

  std::string configFileName(argv[1]);
  std::string solverName(argv[2]);
  std::string meshName;
  std::string dataWriteName;
  std::string dataReadName;

  if (argc != 3) {
    std::cout << "Usage: ./solverdummy configFile solverName\n\n";
    std::cout << "Parameter description\n";
    std::cout << "  configurationFile: Path and filename of preCICE configuration\n";
    std::cout << "  solverName:        SolverDummy participant name in preCICE configuration\n";
    return 1;
  }

  std::cout << "DUMMY: Running solver dummy with preCICE config file \"" << configFileName << "\" and participant name \"" << solverName << "\".\n";

  SolverInterface interface(solverName, configFileName, commRank, commSize);

  if (solverName == "SolverOne") {
    dataWriteName = "dataOne";
    dataReadName  = "dataTwo";
    meshName      = "MeshOne";
  }
  if (solverName == "SolverTwo") {
    dataReadName  = "dataOne";
    dataWriteName = "dataTwo";
    meshName      = "MeshTwo";
  }

  int meshID           = interface.getMeshID(meshName);
  int dimensions       = interface.getDimensions();
  int numberOfVertices = 3;

  const int readDataID  = interface.getDataID(dataReadName, meshID);
  const int writeDataID = interface.getDataID(dataWriteName, meshID);

  std::vector<double> readData(numberOfVertices * dimensions);
  std::vector<double> writeData(numberOfVertices * dimensions);
  std::vector<double> vertices(numberOfVertices * dimensions);
  std::vector<int>    vertexIDs(numberOfVertices);

  for (int i = 0; i < numberOfVertices; i++) {
    for (int j = 0; j < dimensions; j++) {
      vertices.at(j + dimensions * i)  = i;
      readData.at(j + dimensions * i)  = i;
      writeData.at(j + dimensions * i) = i;
    }
  }

  interface.setMeshVertices(meshID, numberOfVertices, vertices.data(), vertexIDs.data());

  double dt = interface.initialize();

  while (interface.isCouplingOngoing()) {

    if (interface.isActionRequired(actionWriteIterationCheckpoint())) {
      std::cout << "DUMMY: Writing iteration checkpoint\n";
      interface.markActionFulfilled(actionWriteIterationCheckpoint());
    }

    if (interface.isReadDataAvailable()) {
      interface.readBlockVectorData(readDataID, numberOfVertices, vertexIDs.data(), readData.data());
    }

    for (int i = 0; i < numberOfVertices * dimensions; i++) {
      writeData.at(i) = readData.at(i) + 1;
    }

    if (interface.isWriteDataRequired(dt)) {
      interface.writeBlockVectorData(writeDataID, numberOfVertices, vertexIDs.data(), writeData.data());
    }

    dt = interface.advance(dt);

    if (interface.isActionRequired(actionReadIterationCheckpoint())) {
      std::cout << "DUMMY: Reading iteration checkpoint\n";
      interface.markActionFulfilled(actionReadIterationCheckpoint());
    } else {
      std::cout << "DUMMY: Advancing in time\n";
    }
  }

  interface.finalize();
  std::cout << "DUMMY: Closing C++ solver dummy...\n";

  return 0;
}
