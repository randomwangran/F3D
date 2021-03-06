/*=========================================================================
  Program:   Visualization Toolkit
  Module:    vtkF3DGenericImporter.cxx
  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.
=========================================================================*/

#include "vtkF3DGenericImporter.h"

#include "F3DLog.h"
#include "F3DOptions.h"

#include <vtkActor.h>
#include <vtkAppendPolyData.h>
#include <vtkBoundingBox.h>
#include <vtkCellData.h>
#include <vtkColorTransferFunction.h>
#include <vtkDataObjectTreeIterator.h>
#include <vtkDataSetSurfaceFilter.h>
#include <vtkDoubleArray.h>
#include <vtkEventForwarderCommand.h>
#include <vtkImageData.h>
#include <vtkImageReader2.h>
#include <vtkImageReader2Factory.h>
#include <vtkImageToPoints.h>
#include <vtkInformation.h>
#include <vtkLightKit.h>
#include <vtkMultiBlockDataSet.h>
#include <vtkObjectFactory.h>
#include <vtkPiecewiseFunction.h>
#include <vtkPointData.h>
#include <vtkPointGaussianMapper.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRectilinearGrid.h>
#include <vtkRectilinearGridToPointSet.h>
#include <vtkRenderer.h>
#include <vtkScalarBarActor.h>
#include <vtkScalarsToColors.h>
#include <vtkSmartPointer.h>
#include <vtkSmartVolumeMapper.h>
#include <vtkStreamingDemandDrivenPipeline.h>
#include <vtkTexture.h>
#include <vtkVertexGlyphFilter.h>
#include <vtkVolumeProperty.h>
#include <vtksys/SystemTools.hxx>

#include "vtkF3DPostProcessFilter.h"

vtkStandardNewMacro(vtkF3DGenericImporter);

//----------------------------------------------------------------------------
void vtkF3DGenericImporter::UpdateTemporalInformation()
{
  if (!this->TemporalInformationUpdated)
  {
    if (!this->Reader->IsReaderValid())
    {
      F3DLog::Print(F3DLog::Severity::Info, "Reader is not valid");
      return;
    }
    this->Reader->UpdateInformation();
    vtkInformation* readerInfo = this->Reader->GetOutputInformation(0);

    this->NbTimeSteps = readerInfo->Length(vtkStreamingDemandDrivenPipeline::TIME_STEPS());
    this->TimeRange = readerInfo->Get(vtkStreamingDemandDrivenPipeline::TIME_RANGE());
    this->TimeSteps = readerInfo->Get(vtkStreamingDemandDrivenPipeline::TIME_STEPS());
    this->TemporalInformationUpdated = true;
  }
}

//----------------------------------------------------------------------------
vtkIdType vtkF3DGenericImporter::GetNumberOfAnimations()
{
  this->UpdateTemporalInformation();
  return this->NbTimeSteps > 0 ? 1 : 0;
}

//----------------------------------------------------------------------------
std::string vtkF3DGenericImporter::GetAnimationName(vtkIdType animationIndex)
{
  return animationIndex < this->GetNumberOfAnimations() ? "default" : "";
}

//----------------------------------------------------------------------------
void vtkF3DGenericImporter::EnableAnimation(vtkIdType animationIndex)
{
  if (animationIndex < this->GetNumberOfAnimations())
  {
    this->AnimationEnabled = true;
  }
}

//----------------------------------------------------------------------------
void vtkF3DGenericImporter::DisableAnimation(vtkIdType animationIndex)
{
  if (animationIndex < this->GetNumberOfAnimations())
  {
    this->AnimationEnabled = false;
  }
}

//----------------------------------------------------------------------------
bool vtkF3DGenericImporter::IsAnimationEnabled(vtkIdType animationIndex)
{
  return animationIndex < this->GetNumberOfAnimations() ? this->AnimationEnabled : false;
}

//----------------------------------------------------------------------------
#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 0, 20201016)
bool vtkF3DGenericImporter::GetTemporalInformation(vtkIdType animationIndex,
  double vtkNotUsed(frameRate), int& nbTimeSteps, double timeRange[2], vtkDoubleArray* timeSteps)
#else
bool vtkF3DGenericImporter::GetTemporalInformation(vtkIdType animationIndex,
  int& nbTimeSteps, double timeRange[2], vtkDoubleArray* timeSteps)
#endif
{
  if (animationIndex < this->GetNumberOfAnimations())
  {
    nbTimeSteps = this->NbTimeSteps;
    timeRange[0] = this->TimeRange[0];
    timeRange[1] = this->TimeRange[1];
    timeSteps->SetArray(this->TimeSteps, this->NbTimeSteps, 1);
    return true;
  }
  return false;
}

//----------------------------------------------------------------------------
void vtkF3DGenericImporter::ImportActors(vtkRenderer* ren)
{
  if (!this->Reader->IsReaderValid())
  {
    F3DLog::Print(
      F3DLog::Severity::Error, "File '", this->Reader->GetFileName(), "' cannot be read.");
    return;
  }

  // forward progress event
  vtkNew<vtkEventForwarderCommand> forwarder;
  forwarder->SetTarget(this);
  this->Reader->AddObserver(vtkCommand::ProgressEvent, forwarder);

  this->PostPro->SetInputConnection(this->Reader->GetOutputPort());
  bool ret = this->PostPro->GetExecutive()->Update();

  if (!ret)
  {
    F3DLog::Print(
      F3DLog::Severity::Error, "File '", this->Reader->GetFileName(), "' cannot be read.");
    return;
  }

  if (this->Options->Verbose || this->Options->NoRender)
  {
    this->OutputDescription =
      vtkF3DGenericImporter::GetDataObjectDescription(this->Reader->GetOutput());
  }

  vtkPolyData* surface = vtkPolyData::SafeDownCast(this->PostPro->GetOutput());
  vtkImageData* image = vtkImageData::SafeDownCast(this->PostPro->GetOutput(2));

  // Configure volume mapper
  this->VolumeMapper->SetInputConnection(this->PostPro->GetOutputPort(2));
  this->VolumeMapper->SetRequestedRenderModeToGPU();

  // Configure polydata mapper
  this->PolyDataMapper->InterpolateScalarsBeforeMappingOn();
  this->PolyDataMapper->SetInputConnection(this->PostPro->GetOutputPort(0));

  // Configure Point Gaussian mapper
  double bounds[6];
  surface->GetBounds(bounds);
  vtkBoundingBox bbox(bounds);
  double pointSize = this->Options->PointSize * bbox.GetDiagonalLength() * 0.001;
  this->PointGaussianMapper->SetInputConnection(this->PostPro->GetOutputPort(1));
  this->PointGaussianMapper->SetScaleFactor(pointSize);
  this->PointGaussianMapper->EmissiveOff();
  this->PointGaussianMapper->SetSplatShaderCode(
    "//VTK::Color::Impl\n"
    "float dist = dot(offsetVCVSOutput.xy, offsetVCVSOutput.xy);\n"
    "if (dist > 1.0) {\n"
    "  discard;\n"
    "} else {\n"
    "  float scale = (1.0 - dist);\n"
    "  ambientColor *= scale;\n"
    "  diffuseColor *= scale;\n"
    "}\n");

  //
  vtkDataSet* dataSet = vtkImageData::SafeDownCast(this->PostPro->GetInput())
    ? vtkDataSet::SafeDownCast(image)
    : vtkDataSet::SafeDownCast(surface);

  std::string usedArray = this->Options->Scalars;
  vtkDataSetAttributes* data = this->Options->Cells
    ? vtkDataSetAttributes::SafeDownCast(dataSet->GetCellData())
    : vtkDataSetAttributes::SafeDownCast(dataSet->GetPointData());

  // Recover an array for coloring if we ever need it
  if (usedArray.empty() || usedArray == f3d::F3DReservedString)
  {
    vtkDataArray* array = data->GetScalars();
    bool print = (this->Options->Verbose || this->Options->NoRender);
    if (array)
    {
      const char* arrayName = array->GetName();
      if (arrayName)
      {
        usedArray = arrayName;
      }
      if (print)
      {
        F3DLog::Print(F3DLog::Severity::Info, "Using default scalar array: ", usedArray);
      }
    }
    else
    {
      if (data->GetNumberOfArrays() > 0)
      {
        array = data->GetArray(0);
      }

      if (array)
      {
        const char* arrayName = array->GetName();
        if (arrayName)
        {
          usedArray = arrayName;
        }
        if (print)
        {
          F3DLog::Print(F3DLog::Severity::Info, "Using first found array: ", usedArray);
        }
      }
      else
      {
        usedArray = "";
        if (print)
        {
          F3DLog::Print(
            F3DLog::Severity::Info, "No array found for scalar coloring and volume rendering");
        }
      }
    }
  }

  // Configure the mappers for coloring if it is ever needed
  vtkDataArray* array = data->GetArray(usedArray.c_str());
  if (array)
  {
    if (this->Options->Component < array->GetNumberOfComponents())
    {
      // Get range
      double range[2];
      if (this->Options->Range.size() == 2)
      {
        range[0] = this->Options->Range[0];
        range[1] = this->Options->Range[1];
      }
      else
      {
        if (this->Options->Range.size() > 0)
        {
          F3DLog::Print(F3DLog::Severity::Warning,
            "The range specified does not have exactly 2 values, using automatic range.");
        }
        array->GetRange(range, this->Options->Component);
      }

      // Create lookup table
      vtkNew<vtkColorTransferFunction> ctf;
      if (this->Options->LookupPoints.size() > 0)
      {
        if (this->Options->LookupPoints.size() % 4 == 0)
        {
          for (size_t i = 0; i < this->Options->LookupPoints.size(); i += 4)
          {
            double val = this->Options->LookupPoints[i];
            double r = this->Options->LookupPoints[i + 1];
            double g = this->Options->LookupPoints[i + 2];
            double b = this->Options->LookupPoints[i + 3];
            ctf->AddRGBPoint(range[0] + val * (range[1] - range[0]), r, g, b);
          }
        }
        else
        {
          F3DLog::Print(F3DLog::Severity::Warning,
            "Specified color map list count is not a multiple of 4, ignoring it.");
        }
      }

      if (this->Options->Component >= 0)
      {
        ctf->SetVectorModeToComponent();
        ctf->SetVectorComponent(this->Options->Component);
      }
      else
      {
        ctf->SetVectorModeToMagnitude();
      }

      if (this->VolumeMapper->GetInput())
      {
        this->ConfigureVolumeForColoring(this->VolumeMapper, this->VolumeProp, array, ctf, range);
      }

      this->ConfigureMapperForColoring(this->PointGaussianMapper, array, ctf, range);
      this->ConfigureMapperForColoring(this->PolyDataMapper, array, ctf, range);

      std::string title = usedArray;
      if (this->Options->Component >= 0)
      {
        title += " (Component #";
        title += std::to_string(this->Options->Component);
        title += ")";
      }

      this->ScalarBarActor->SetLookupTable(ctf);
      this->ScalarBarActor->SetTitle(title.c_str());
      this->ScalarBarActor->SetNumberOfLabels(4);
      this->ScalarBarActor->SetOrientationToHorizontal();
      this->ScalarBarActor->SetWidth(0.8);
      this->ScalarBarActor->SetHeight(0.07);
      this->ScalarBarActor->SetPosition(0.1, 0.01);
      ren->AddActor2D(this->ScalarBarActor);

      this->ScalarsAvailable = true;
    }
    else
    {
      F3DLog::Print(
        F3DLog::Severity::Warning, "Invalid component index: ", this->Options->Component);
      this->ScalarsAvailable = false;
    }
  }
  else
  {
    this->ScalarsAvailable = false;
    this->PointGaussianMapper->ScalarVisibilityOff();
    this->PolyDataMapper->ScalarVisibilityOff();

    if (!usedArray.empty() && !(usedArray == f3d::F3DReservedString))
    {
      F3DLog::Print(F3DLog::Severity::Warning, "Unknow scalar array: ", usedArray);
    }
  }

  // configure props
  this->VolumeProp->SetMapper(this->VolumeMapper);

  this->GeometryActor->SetMapper(this->PolyDataMapper);
  this->GeometryActor->GetProperty()->SetInterpolationToPBR();

  double col[3];
  std::copy(this->Options->SolidColor.begin(), this->Options->SolidColor.end(), col);

  this->GeometryActor->GetProperty()->SetColor(col);
  this->GeometryActor->GetProperty()->SetOpacity(this->Options->Opacity);
  this->GeometryActor->GetProperty()->SetRoughness(this->Options->Roughness);
  this->GeometryActor->GetProperty()->SetMetallic(this->Options->Metallic);
  this->GeometryActor->GetProperty()->SetPointSize(this->Options->PointSize);
  this->GeometryActor->GetProperty()->SetLineWidth(this->Options->LineWidth);

  this->PointSpritesActor->SetMapper(this->PointGaussianMapper);
  this->PointSpritesActor->GetProperty()->SetColor(col);
  this->PointSpritesActor->GetProperty()->SetOpacity(this->Options->Opacity);

  // Textures
  this->GeometryActor->GetProperty()->SetBaseColorTexture(
    this->GetTexture(this->Options->BaseColorTex, true));
  this->GeometryActor->GetProperty()->SetORMTexture(this->GetTexture(this->Options->ORMTex));
  this->GeometryActor->GetProperty()->SetEmissiveTexture(
    this->GetTexture(this->Options->EmissiveTex, true));
  this->GeometryActor->GetProperty()->SetEmissiveFactor(this->Options->EmissiveFactor.data());
  this->GeometryActor->GetProperty()->SetNormalTexture(this->GetTexture(this->Options->NormalTex));
  this->GeometryActor->GetProperty()->SetNormalScale(this->Options->NormalScale);

  // add props
  ren->AddActor(this->GeometryActor);
  ren->AddActor(this->PointSpritesActor);
  ren->AddVolume(this->VolumeProp);
}

//----------------------------------------------------------------------------
// TODO : add this function in a utils file for rendering in VTK directly
vtkSmartPointer<vtkTexture> vtkF3DGenericImporter::GetTexture(
  const std::string& fileName, bool isSRGB)
{
  vtkSmartPointer<vtkTexture> texture;
  if (!fileName.empty())
  {
    std::string fullPath = vtksys::SystemTools::CollapseFullPath(fileName);

    auto reader = vtkSmartPointer<vtkImageReader2>::Take(
      vtkImageReader2Factory::CreateImageReader2(fullPath.c_str()));
    if (reader)
    {
      reader->SetFileName(fullPath.c_str());
      reader->Update();
      texture = vtkSmartPointer<vtkTexture>::New();
      texture->SetInputConnection(reader->GetOutputPort());
      if (isSRGB)
      {
        texture->UseSRGBColorSpaceOn();
      }
      texture->InterpolateOn();
      return texture;
    }
    else
    {
      vtkWarningMacro("Cannot open texture file " << fullPath);
    }
  }

  return texture;
}

//----------------------------------------------------------------------------
void vtkF3DGenericImporter::ConfigureVolumeForColoring(vtkSmartVolumeMapper* mapper,
  vtkVolume* volume, vtkDataArray* array, vtkColorTransferFunction* ctf, double range[2])
{
  mapper->SetScalarMode(this->Options->Cells ? VTK_SCALAR_MODE_USE_CELL_FIELD_DATA
                                             : VTK_SCALAR_MODE_USE_POINT_FIELD_DATA);
  mapper->SelectScalarArray(array->GetName());

  // comp > 4 is actually not supported and will fail with a vtk error
  if (this->Options->Component >= 0)
  {
    mapper->SetVectorMode(vtkSmartVolumeMapper::COMPONENT);
    mapper->SetVectorComponent(this->Options->Component);
  }
  else if (this->Options->Component == -1)
  {
    mapper->SetVectorMode(vtkSmartVolumeMapper::MAGNITUDE);
  }
  else if (this->Options->Component == -2)
  {
    mapper->SetVectorMode(vtkSmartVolumeMapper::DISABLED);
  }

  vtkNew<vtkPiecewiseFunction> otf;
  otf->AddPoint(range[0], this->Options->InverseOpacityFunction ? 1.0 : 0.0);
  otf->AddPoint(range[1], this->Options->InverseOpacityFunction ? 0.0 : 1.0);

  vtkNew<vtkVolumeProperty> property;
  property->SetColor(ctf);
  property->SetScalarOpacity(otf);
  property->ShadeOff();
  property->SetInterpolationTypeToLinear();

  volume->SetProperty(property);
}

//----------------------------------------------------------------------------
void vtkF3DGenericImporter::ConfigureMapperForColoring(
  vtkPolyDataMapper* mapper, vtkDataArray* array, vtkColorTransferFunction* ctf, double range[2])
{
  mapper->SetColorModeToMapScalars();
  mapper->SelectColorArray(array->GetName());
  mapper->SetScalarMode(this->Options->Cells ? VTK_SCALAR_MODE_USE_CELL_FIELD_DATA
                                             : VTK_SCALAR_MODE_USE_POINT_FIELD_DATA);
  mapper->ScalarVisibilityOn();

  if (this->Options->Component == -2)
  {
    mapper->SetColorModeToDirectScalars();
  }
  else
  {
    mapper->SetColorModeToMapScalars();
    mapper->SetScalarRange(range);
    mapper->SetLookupTable(ctf);
  }
}

//----------------------------------------------------------------------------
void vtkF3DGenericImporter::ImportLights(vtkRenderer* ren)
{
  ren->RemoveAllLights();
  ren->AutomaticLightCreationOff();

  if (!ren->GetUseImageBasedLighting())
  {
    vtkNew<vtkLightKit> lightKit;
    lightKit->AddLightsToRenderer(ren);
  }
}

//----------------------------------------------------------------------------
void vtkF3DGenericImporter::ImportProperties(vtkRenderer* vtkNotUsed(ren)) {}

//----------------------------------------------------------------------------
void vtkF3DGenericImporter::PrintSelf(std::ostream& os, vtkIndent indent)
{
  vtkImporter::PrintSelf(os, indent);
}

//----------------------------------------------------------------------------
void vtkF3DGenericImporter::SetFileName(const char* arg)
{
  this->TemporalInformationUpdated = false;
  this->Reader->SetFileName(std::string(arg));
}

//----------------------------------------------------------------------------
void vtkF3DGenericImporter::SetOptions(const F3DOptions& options)
{
  this->Options = &options;
}

//----------------------------------------------------------------------------
bool vtkF3DGenericImporter::CanReadFile()
{
  return this->Reader->IsReaderValid();
}

//----------------------------------------------------------------------------
std::string vtkF3DGenericImporter::GetOutputsDescription()
{
  return this->OutputDescription;
}

//----------------------------------------------------------------------------
std::string vtkF3DGenericImporter::GetMultiBlockDescription(
  vtkMultiBlockDataSet* mb, vtkIndent indent)
{
  std::stringstream ss;
  for (vtkIdType i = 0; i < mb->GetNumberOfBlocks(); i++)
  {
    const char* blockName = mb->GetMetaData(i)->Get(vtkCompositeDataSet::NAME());
    ss << indent << "Block: " << (blockName ? std::string(blockName) : std::to_string(i)) << "\n";
    vtkDataObject* object = mb->GetBlock(i);
    vtkMultiBlockDataSet* mbChild = vtkMultiBlockDataSet::SafeDownCast(object);
    vtkDataSet* ds = vtkDataSet::SafeDownCast(object);
    if (mbChild)
    {
      ss << vtkF3DGenericImporter::GetMultiBlockDescription(mbChild, indent.GetNextIndent());
    }
    else if (ds)
    {
      ss << vtkImporter::GetDataSetDescription(ds, indent.GetNextIndent());
    }
  }
  return ss.str();
}

//----------------------------------------------------------------------------
std::string vtkF3DGenericImporter::GetDataObjectDescription(vtkDataObject* object)
{
  vtkMultiBlockDataSet* mb = vtkMultiBlockDataSet::SafeDownCast(object);
  vtkDataSet* ds = vtkDataSet::SafeDownCast(object);
  if (mb)
  {
    return vtkF3DGenericImporter::GetMultiBlockDescription(mb, vtkIndent(0));
  }
  else if (ds)
  {
    return vtkImporter::GetDataSetDescription(ds, vtkIndent(0));
  }
  return "";
}

//----------------------------------------------------------------------------
void vtkF3DGenericImporter::UpdateTimeStep(double timestep)
{
  this->PostPro->UpdateTimeStep(timestep);
}
