// Copyright (c) 2013 Vasili Baranau
// Distributed under the MIT software license
// See the accompanying file License.txt or http://opensource.org/licenses/MIT

#include "../Headers/InsertionRadiiGenerator.h"

#include <ctime>
#include <stdio.h>
#include "Generation/PackingServices/DistanceServices/Headers/DistanceService.h"
#include "Generation/PackingServices/Headers/GeometryService.h"
#include "Generation/PackingServices/Headers/PackingSerializer.h"
#include "Generation/PackingServices/EnergyServices/Headers/IEnergyService.h"
#include "Generation/PackingServices/EnergyServices/Headers/HarmonicPotential.h"
#include "Generation/Model/Headers/Config.h"
#include "Generation/Geometries/Headers/IGeometry.h"
#include "Core/Headers/VectorUtilities.h"
#include "Core/Headers/StlUtilities.h"
#include "Core/Headers/Constants.h"

using namespace std;
using namespace Core;
using namespace Model;

namespace PackingServices
{
    InsertionRadiiGenerator::InsertionRadiiGenerator(DistanceService* distanceProvider, GeometryService* geometryService)
    {
        this->distanceProvider = distanceProvider;
        this->geometryService = geometryService;
    }

    InsertionRadiiGenerator::~InsertionRadiiGenerator()
    {

    }

    void InsertionRadiiGenerator::SetContext(const ModellingContext& context)
    {
        this->context = &context;
        geometry = context.geometry;
        config = context.config;
        distanceProvider->SetContext(context);
        geometryService->SetContext(context);
    }

    void InsertionRadiiGenerator::FillDistancesToSurfaces(const Packing& particles, int samplePointsCount, const vector<int>& sortedSurfaceIndexes, string distancesFolderPath, const PackingSerializer& packingSerializer) const
    {
        clock_t startTime = clock();

        int loggingPeriod = 100000;
        if (loggingPeriod > samplePointsCount)
        {
            loggingPeriod = samplePointsCount;
        }
        distanceProvider->SetParticles(particles);
        vector<vector<FLOAT_TYPE> > distancesToSurfaces(loggingPeriod, vector<FLOAT_TYPE>(sortedSurfaceIndexes.size()));

        FLOAT_TYPE meanDiameter = geometryService->GetMeanParticleDiameter(particles);

        vector<FLOAT_TYPE> unfilteredDistancesToSurfaces;
        int maxSurfaceIndex = sortedSurfaceIndexes[sortedSurfaceIndexes.size() - 1];

        int savedPointsCount = 0;
        for (int samplePointIndex = 0; samplePointIndex < samplePointsCount; ++samplePointIndex)
        {
            SpatialVector point;
            FillRandomPoint(&point);

            distanceProvider->FillDistancesToClosestSurfaces(point, &unfilteredDistancesToSurfaces);

            // This code is exactly two times faster than direct sorting of the entire unfilteredDistancesToSurfaces.
            // Indeed, SortByNthElement is linear, unfilteredDistancesToSurfaces.size() is about 230, while maxSurfaceIndex is about 13.
            // So 230 + 13 * log(13) =  263.3443, 230 * log(230) = 1250.8
            StlUtilities::SortByNthElement(&unfilteredDistancesToSurfaces, maxSurfaceIndex);
            unfilteredDistancesToSurfaces.resize(maxSurfaceIndex);
            StlUtilities::Sort(&unfilteredDistancesToSurfaces);

            vector<FLOAT_TYPE>& currentDistancesToSurfaces = distancesToSurfaces[samplePointIndex - savedPointsCount];
            for (size_t i = 0; i < sortedSurfaceIndexes.size(); ++i)
            {
                currentDistancesToSurfaces[i] = unfilteredDistancesToSurfaces[sortedSurfaceIndexes[i]];
            }

            VectorUtilities::DivideByValue(currentDistancesToSurfaces, meanDiameter, &currentDistancesToSurfaces);

            bool shouldLog = ((samplePointIndex + 1) % loggingPeriod == 0) && (samplePointIndex > 0);
            bool isLastPoint = samplePointIndex == samplePointsCount - 1;
            if (shouldLog || isLastPoint)
            {
                bool shouldAppend = savedPointsCount > 0;
                packingSerializer.SerializeDistancesToSurfaces(distancesFolderPath, sortedSurfaceIndexes, distancesToSurfaces, shouldAppend);
                savedPointsCount += loggingPeriod;
                int leftPointsCount = samplePointsCount - savedPointsCount;
                // This may only happen if we are left with the last chunk of points.
                if (leftPointsCount < loggingPeriod)
                {
                    distancesToSurfaces.resize(leftPointsCount);
                }
                printf("Generated %g pro cent of sample points\n", 100.0 * (samplePointIndex + 1) / samplePointsCount);
            }
        }

        clock_t endTime = clock();
        printf("Elapsed time is %g sec\n", static_cast<FLOAT_TYPE>(endTime - startTime) / CLOCKS_PER_SEC);
    }

    FLOAT_TYPE InsertionRadiiGenerator::GetContractionRateForCoordinationNumber(IEnergyService* energyService, FLOAT_TYPE expectedAverageCoordinationNumber)
    {
        vector<FLOAT_TYPE> contractionRatios;
        boost::array<FLOAT_TYPE, 2> contractionRatiosArray = {{1.0, 0.999}};
        StlUtilities::ResizeAndCopy(contractionRatiosArray, &contractionRatios);
        const HarmonicPotential zeroPotential(0.0);
        vector<const IPairPotential*> potentials(contractionRatios.size(), &zeroPotential);

        IEnergyService::EnergiesResult result = energyService->GetContractionEnergies(contractionRatios, potentials);
//        if (result.contractionEnergies[0] / result.nonRattlersCounts[0] > expectedAverageCoordinationNumber)
//        {
//            throw InvalidOperationException("Coordination number without contraction is larger than the expected one");
//        }
        if (result.contractionEnergies[1] / result.nonRattlersCounts[1] < expectedAverageCoordinationNumber)
        {
            throw InvalidOperationException("Coordination number with max contraction is lower than the expected one");
        }

        // Contraction energy decreases with the increase of contractionRatio.
        // We need an inverse behavior for DoBinarySearch.
        FLOAT_TYPE leftNegativeContractionRatio = -contractionRatios[0];
        FLOAT_TYPE rightNegativeContractionRatio = -contractionRatios[1];

        const FLOAT_TYPE maxError = 1e-4;

        contractionRatios.resize(1);
        potentials.resize(1);
        GetContractionEnergyFunctor getContractionEnergyFunctor(potentials, energyService);
        FLOAT_TYPE negativeContractionRatio = StlUtilities::DoBinarySearch(leftNegativeContractionRatio, rightNegativeContractionRatio, expectedAverageCoordinationNumber, maxError, 0.0, getContractionEnergyFunctor);
        return -negativeContractionRatio;
    }

    FLOAT_TYPE InsertionRadiiGenerator::GetContactNumberDistribution(const Packing& particles, IEnergyService* energyService, FLOAT_TYPE contractionRate, vector<int>* neighborCounts, vector<int>* neighborCountFrequencies) const
    {
        vector<FLOAT_TYPE> contractionRatios(1, contractionRate);
        const HarmonicPotential zeroPotential(0.0);
        vector<const IPairPotential*> potentials(1, &zeroPotential);

        vector<IEnergyService::EnergiesPerParticle> energiesPerParticleVector;
        energyService->GetContractionEnergiesPerParticle(contractionRatios, potentials, &energiesPerParticleVector);

        IEnergyService::EnergiesPerParticle& energiesPerParticle = energiesPerParticleVector[0];
        vector<int> neighborCountsPerParticle(energiesPerParticle.contractionEnergiesPerParticle.size());
        VectorUtilities::Round(energiesPerParticle.contractionEnergiesPerParticle, &neighborCountsPerParticle);

        int maxNeighborCountPosition = StlUtilities::FindMaxElementPosition(neighborCountsPerParticle);
        int maxNeighborCount = neighborCountsPerParticle[maxNeighborCountPosition];

        vector<int> fullNeighborCountFrequencies(maxNeighborCount + 1, 0);
        for (size_t i = 0; i < neighborCountsPerParticle.size(); ++i)
        {
            if (!energiesPerParticle.rattlerMask[i] && !particles[i].isImmobile)
            {
                fullNeighborCountFrequencies[neighborCountsPerParticle[i]]++;
            }
        }

        neighborCounts->empty();
        neighborCountFrequencies->empty();
        for (size_t i = 0; i < fullNeighborCountFrequencies.size(); ++i)
        {
            if (fullNeighborCountFrequencies[i] > 0)
            {
                neighborCounts->push_back(i);
                neighborCountFrequencies->push_back(fullNeighborCountFrequencies[i]);
            }
        }

        vector<FLOAT_TYPE> contacts(neighborCounts->size());
        VectorUtilities::Multiply(*neighborCounts, *neighborCountFrequencies, &contacts);
        FLOAT_TYPE totalContacts = VectorUtilities::Sum(contacts);
        FLOAT_TYPE nonRattlerCounts = VectorUtilities::Sum(*neighborCountFrequencies);
        FLOAT_TYPE coordinationNumber = totalContacts / nonRattlerCounts;
        return coordinationNumber;
    }

    void InsertionRadiiGenerator::FillInsertionRadii(const Packing& particles, int insertionRadiiCount, vector<FLOAT_TYPE>* insertionRadii) const
    {
        distanceProvider->SetParticles(particles);
        insertionRadii->reserve(insertionRadiiCount);

        FLOAT_TYPE meanDiameter = geometryService->GetMeanParticleDiameter(particles);

        for (int i = 0; i < insertionRadiiCount; ++i)
        {
            SpatialVector point;
            FillRandomPoint(&point);

            FLOAT_TYPE insertionRadius = distanceProvider->GetDistanceToNearestSurface(point) / meanDiameter;
            insertionRadii->push_back(insertionRadius);
        }
    }

    FLOAT_TYPE InsertionRadiiGenerator::CalculateEntropy(const Packing& particles, int minInsertionRadiiCount) const
    {
        // Insertion radii are normalized by meanDiameter, which corresponds to rescaling the packing to make particles have mean radius = 0.5.
        // Therefore we can used a fixed samplePoreRadius and meanParticleRadius.
        const FLOAT_TYPE minPoreRadius = 0.25;
        const FLOAT_TYPE meanParticleRadius = 0.5;
        const FLOAT_TYPE normalizationFactor = (meanParticleRadius / minPoreRadius) * (meanParticleRadius / minPoreRadius);

        distanceProvider->SetParticles(particles);
        FLOAT_TYPE meanDiameter = geometryService->GetMeanParticleDiameter(particles);

        const int checkPeriod = 10000;
        const FLOAT_TYPE relativeTolerance = 1e-3; // entropy values -20 and -21 are acceptably close

        int insertionRadiiCount = 0;
        int largePoresCount = 0;
        FLOAT_TYPE entropy = 0;
        FLOAT_TYPE previousEntropy = 0;

        while (1)
        {
            if (insertionRadiiCount % checkPeriod == 0 && insertionRadiiCount > 0)
            {
                FLOAT_TYPE largePoresDensity = static_cast<FLOAT_TYPE>(largePoresCount) / insertionRadiiCount;
                entropy = log(largePoresDensity) * normalizationFactor;
                bool toleranceCorrect = std::abs((previousEntropy - entropy) / entropy) < relativeTolerance;
                if (toleranceCorrect && insertionRadiiCount >= minInsertionRadiiCount)
                {
                    break;
                }
                else
                {
                    previousEntropy = entropy;
                }
            }

            SpatialVector point;
            FillRandomPoint(&point);

            FLOAT_TYPE poreRadius = distanceProvider->GetDistanceToNearestSurface(point) / meanDiameter;
            if (poreRadius > minPoreRadius)
            {
                largePoresCount++;
            }

            insertionRadiiCount++;
        }

        printf("insertionRadiiCount: %d\n", insertionRadiiCount);
        FLOAT_TYPE largePoresDensity = static_cast<FLOAT_TYPE>(largePoresCount) / insertionRadiiCount;
        entropy = log(largePoresDensity) * normalizationFactor;

        return entropy;
    }

    void InsertionRadiiGenerator::FillRandomPoint(Core::SpatialVector* point) const
    {
        while (true)
        {
            VectorUtilities::InitializeWithRandoms(point);
            VectorUtilities::Multiply(*point, config->packingSize, point);
            bool isPointCorrect = geometry->IsSphereInside(*point, 0);
            if (isPointCorrect && this->context->activeGeometry != NULL)
            {
                // Not very efficient. Probably have to add a method "GenerateRandomPoint" to IGeometry
                // (but i can't use it anyway for PoissonInCellsGenerator. Thus, usage in PoissonGenerator will be inconsistent).
                isPointCorrect &= this->context->activeGeometry->IsSphereInside(*point, 0);
            }

            if (isPointCorrect)
            {
                return;
            }
        };
    }
}

