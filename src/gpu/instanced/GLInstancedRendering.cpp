/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GLInstancedRendering.h"

#include "GrResourceProvider.h"
#include "gl/GrGLGpu.h"
#include "instanced/InstanceProcessor.h"

#define GL_CALL(X) GR_GL_CALL(this->glGpu()->glInterface(), X)

namespace gr_instanced {

class GLInstancedRendering::GLBatch : public InstancedRendering::Batch {
public:
    DEFINE_BATCH_CLASS_ID

    GLBatch(GLInstancedRendering* instRendering) : INHERITED(ClassID(), instRendering) {}
    int numGLCommands() const { return 1 + fNumChangesInGeometry; }

private:
    int fEmulatedBaseInstance;
    int fGLDrawCmdsIdx;

    friend class GLInstancedRendering;

    typedef Batch INHERITED;
};

GLInstancedRendering* GLInstancedRendering::CreateIfSupported(GrGLGpu* gpu) {
#ifndef SK_BUILD_FOR_MAC
    // Only whitelisting on Mac for now. Once we've been able to work through the various issues on
    // other platforms we can enable more generally.
    return nullptr;
#endif
    const GrGLCaps& glCaps = gpu->glCaps();
    AntialiasMode lastSupportedAAMode;
    if (!glCaps.vertexArrayObjectSupport() ||
        !glCaps.drawIndirectSupport() ||
        !InstanceProcessor::IsSupported(*glCaps.glslCaps(), glCaps, &lastSupportedAAMode)) {
        return nullptr;
    }
    return new GLInstancedRendering(gpu, lastSupportedAAMode);
}

GLInstancedRendering::GLInstancedRendering(GrGLGpu* gpu, AntialiasMode lastSupportedAAMode)
    : INHERITED(gpu, lastSupportedAAMode, gpu->glCaps().canDrawIndirectToFloat()),
      fVertexArrayID(0),
      fGLDrawCmdsInfo(0),
      fInstanceAttribsBufferUniqueId(SK_InvalidUniqueID) {
}

GLInstancedRendering::~GLInstancedRendering() {
    if (fVertexArrayID) {
        GL_CALL(DeleteVertexArrays(1, &fVertexArrayID));
        this->glGpu()->notifyVertexArrayDelete(fVertexArrayID);
    }
}

inline GrGLGpu* GLInstancedRendering::glGpu() const {
    return static_cast<GrGLGpu*>(this->gpu());
}

InstancedRendering::Batch* GLInstancedRendering::createBatch() {
    return new GLBatch(this);
}

void GLInstancedRendering::onBeginFlush(GrResourceProvider* rp) {
    // Count what there is to draw.
    BatchList::Iter iter;
    iter.init(this->trackedBatches(), BatchList::Iter::kHead_IterStart);
    int numGLInstances = 0;
    int numGLDrawCmds = 0;
    while (Batch* b = iter.get()) {
        GLBatch* batch = static_cast<GLBatch*>(b);
        iter.next();

        numGLInstances += batch->fNumDraws;
        numGLDrawCmds += batch->numGLCommands();
    }
    if (!numGLDrawCmds) {
        return;
    }
    SkASSERT(numGLInstances);

    // Lazily create a vertex array object.
    if (!fVertexArrayID) {
        GL_CALL(GenVertexArrays(1, &fVertexArrayID));
        if (!fVertexArrayID) {
            return;
        }
        this->glGpu()->bindVertexArray(fVertexArrayID);

        // Attach our index buffer to the vertex array.
        GL_CALL(BindBuffer(GR_GL_ELEMENT_ARRAY_BUFFER,
                           static_cast<const GrGLBuffer*>(this->indexBuffer())->bufferID()));

        // Set up the non-instanced attribs.
        this->glGpu()->bindBuffer(kVertex_GrBufferType,
                                  static_cast<const GrGLBuffer*>(this->vertexBuffer()));
        GL_CALL(EnableVertexAttribArray((int)Attrib::kShapeCoords));
        GL_CALL(VertexAttribPointer((int)Attrib::kShapeCoords, 2, GR_GL_FLOAT, GR_GL_FALSE,
                                    sizeof(ShapeVertex), (void*) offsetof(ShapeVertex, fX)));
        GL_CALL(EnableVertexAttribArray((int)Attrib::kVertexAttrs));
        GL_CALL(VertexAttribIPointer((int)Attrib::kVertexAttrs, 1, GR_GL_INT, sizeof(ShapeVertex),
                                     (void*) offsetof(ShapeVertex, fAttrs)));

        SkASSERT(SK_InvalidUniqueID == fInstanceAttribsBufferUniqueId);
    }

    // Create and map instance and draw-indirect buffers.
    SkASSERT(!fInstanceBuffer);
    fInstanceBuffer.reset(static_cast<GrGLBuffer*>(
        rp->createBuffer(sizeof(Instance) * numGLInstances, kVertex_GrBufferType,
                         kDynamic_GrAccessPattern, GrResourceProvider::kNoPendingIO_Flag)));
    if (!fInstanceBuffer) {
        return;
    }

    SkASSERT(!fDrawIndirectBuffer);
    fDrawIndirectBuffer.reset(static_cast<GrGLBuffer*>(
        rp->createBuffer(sizeof(GrGLDrawElementsIndirectCommand) * numGLDrawCmds,
                         kDrawIndirect_GrBufferType, kDynamic_GrAccessPattern,
                         GrResourceProvider::kNoPendingIO_Flag)));
    if (!fDrawIndirectBuffer) {
        return;
    }

    Instance* glMappedInstances = static_cast<Instance*>(fInstanceBuffer->map());
    int glInstancesIdx = 0;

    auto* glMappedCmds = static_cast<GrGLDrawElementsIndirectCommand*>(fDrawIndirectBuffer->map());
    int glDrawCmdsIdx = 0;

    bool baseInstanceSupport = this->glGpu()->glCaps().baseInstanceSupport();

    if (GR_GL_LOG_INSTANCED_BATCHES || !baseInstanceSupport) {
        fGLDrawCmdsInfo.reset(numGLDrawCmds);
    }

    // Generate the instance and draw-indirect buffer contents based on the tracked batches.
    iter.init(this->trackedBatches(), BatchList::Iter::kHead_IterStart);
    while (Batch* b = iter.get()) {
        GLBatch* batch = static_cast<GLBatch*>(b);
        iter.next();

        batch->fEmulatedBaseInstance = baseInstanceSupport ? 0 : glInstancesIdx;
        batch->fGLDrawCmdsIdx = glDrawCmdsIdx;

        const Batch::Draw* draw = batch->fHeadDraw;
        SkASSERT(draw);
        do {
            int instanceCount = 0;
            IndexRange geometry = draw->fGeometry;
            SkASSERT(!geometry.isEmpty());

            do {
                glMappedInstances[glInstancesIdx + instanceCount++] = draw->fInstance;
                draw = draw->fNext;
            } while (draw && draw->fGeometry == geometry);

            GrGLDrawElementsIndirectCommand& glCmd = glMappedCmds[glDrawCmdsIdx];
            glCmd.fCount = geometry.fCount;
            glCmd.fInstanceCount = instanceCount;
            glCmd.fFirstIndex = geometry.fStart;
            glCmd.fBaseVertex = 0;
            glCmd.fBaseInstance = baseInstanceSupport ? glInstancesIdx : 0;

            if (GR_GL_LOG_INSTANCED_BATCHES || !baseInstanceSupport) {
                fGLDrawCmdsInfo[glDrawCmdsIdx].fInstanceCount = instanceCount;
#if GR_GL_LOG_INSTANCED_BATCHES
                fGLDrawCmdsInfo[glDrawCmdsIdx].fGeometry = geometry;
#endif
            }

            glInstancesIdx += instanceCount;
            ++glDrawCmdsIdx;
        } while (draw);
    }

    SkASSERT(glDrawCmdsIdx == numGLDrawCmds);
    fDrawIndirectBuffer->unmap();

    SkASSERT(glInstancesIdx == numGLInstances);
    fInstanceBuffer->unmap();
}

void GLInstancedRendering::onDraw(const GrPipeline& pipeline, const InstanceProcessor& instProc,
                                  const Batch* baseBatch) {
    if (!fDrawIndirectBuffer) {
        return; // beginFlush was not successful.
    }
    if (!this->glGpu()->flushGLState(pipeline, instProc)) {
        return;
    }

    this->glGpu()->bindBuffer(kDrawIndirect_GrBufferType, fDrawIndirectBuffer.get());

    const GrGLCaps& glCaps = this->glGpu()->glCaps();
    const GLBatch* batch = static_cast<const GLBatch*>(baseBatch);
    int numCommands = batch->numGLCommands();

#if GR_GL_LOG_INSTANCED_BATCHES
    SkASSERT(fGLDrawCmdsInfo);
    SkDebugf("Instanced batch: [");
    for (int i = 0; i < numCommands; ++i) {
        int glCmdIdx = batch->fGLDrawCmdsIdx + i;
        SkDebugf("%s%i * %s", (i ? ",  " : ""), fGLDrawCmdsInfo[glCmdIdx].fInstanceCount,
                 InstanceProcessor::GetNameOfIndexRange(fGLDrawCmdsInfo[glCmdIdx].fGeometry));
    }
    SkDebugf("]\n");
#else
    SkASSERT(SkToBool(fGLDrawCmdsInfo) == !glCaps.baseInstanceSupport());
#endif

    if (1 == numCommands || !glCaps.baseInstanceSupport() || !glCaps.multiDrawIndirectSupport()) {
        int emulatedBaseInstance = batch->fEmulatedBaseInstance;
        for (int i = 0; i < numCommands; ++i) {
            int glCmdIdx = batch->fGLDrawCmdsIdx + i;
            this->flushInstanceAttribs(emulatedBaseInstance);
            GL_CALL(DrawElementsIndirect(GR_GL_TRIANGLES, GR_GL_UNSIGNED_BYTE,
                                         (GrGLDrawElementsIndirectCommand*) nullptr + glCmdIdx));
            if (!glCaps.baseInstanceSupport()) {
                emulatedBaseInstance += fGLDrawCmdsInfo[glCmdIdx].fInstanceCount;
            }
        }
    } else {
        int glCmdsIdx = batch->fGLDrawCmdsIdx;
        this->flushInstanceAttribs(batch->fEmulatedBaseInstance);
        GL_CALL(MultiDrawElementsIndirect(GR_GL_TRIANGLES, GR_GL_UNSIGNED_BYTE,
                                          (GrGLDrawElementsIndirectCommand*) nullptr + glCmdsIdx,
                                          numCommands, 0));
    }
}

void GLInstancedRendering::flushInstanceAttribs(int baseInstance) {
    SkASSERT(fVertexArrayID);
    this->glGpu()->bindVertexArray(fVertexArrayID);

    SkASSERT(fInstanceBuffer);
    if (fInstanceAttribsBufferUniqueId != fInstanceBuffer->getUniqueID() ||
        fInstanceAttribsBaseInstance != baseInstance) {
        Instance* offsetInBuffer = (Instance*) nullptr + baseInstance;

        this->glGpu()->bindBuffer(kVertex_GrBufferType, fInstanceBuffer.get());

        // Info attrib.
        GL_CALL(EnableVertexAttribArray((int)Attrib::kInstanceInfo));
        GL_CALL(VertexAttribIPointer((int)Attrib::kInstanceInfo, 1, GR_GL_UNSIGNED_INT,
                                     sizeof(Instance), &offsetInBuffer->fInfo));
        GL_CALL(VertexAttribDivisor((int)Attrib::kInstanceInfo, 1));

        // Shape matrix attrib.
        GL_CALL(EnableVertexAttribArray((int)Attrib::kShapeMatrixX));
        GL_CALL(EnableVertexAttribArray((int)Attrib::kShapeMatrixY));
        GL_CALL(VertexAttribPointer((int)Attrib::kShapeMatrixX, 3, GR_GL_FLOAT, GR_GL_FALSE,
                                    sizeof(Instance), &offsetInBuffer->fShapeMatrix2x3[0]));
        GL_CALL(VertexAttribPointer((int)Attrib::kShapeMatrixY, 3, GR_GL_FLOAT, GR_GL_FALSE,
                                    sizeof(Instance), &offsetInBuffer->fShapeMatrix2x3[3]));
        GL_CALL(VertexAttribDivisor((int)Attrib::kShapeMatrixX, 1));
        GL_CALL(VertexAttribDivisor((int)Attrib::kShapeMatrixY, 1));

        // Color attrib.
        GL_CALL(EnableVertexAttribArray((int)Attrib::kColor));
        GL_CALL(VertexAttribPointer((int)Attrib::kColor, 4, GR_GL_UNSIGNED_BYTE, GR_GL_TRUE,
                                    sizeof(Instance), &offsetInBuffer->fColor));
        GL_CALL(VertexAttribDivisor((int)Attrib::kColor, 1));

        // Local rect attrib.
        GL_CALL(EnableVertexAttribArray((int)Attrib::kLocalRect));
        GL_CALL(VertexAttribPointer((int)Attrib::kLocalRect, 4, GR_GL_FLOAT, GR_GL_FALSE,
                                    sizeof(Instance), &offsetInBuffer->fLocalRect));
        GL_CALL(VertexAttribDivisor((int)Attrib::kLocalRect, 1));

        fInstanceAttribsBufferUniqueId = fInstanceBuffer->getUniqueID();
        fInstanceAttribsBaseInstance = baseInstance;
    }
}

void GLInstancedRendering::onEndFlush() {
    fInstanceBuffer.reset();
    fDrawIndirectBuffer.reset();
    fGLDrawCmdsInfo.reset(0);
}

void GLInstancedRendering::onResetGpuResources(ResetType resetType) {
    if (fVertexArrayID && ResetType::kDestroy == resetType) {
        GL_CALL(DeleteVertexArrays(1, &fVertexArrayID));
        this->glGpu()->notifyVertexArrayDelete(fVertexArrayID);
    }
    fVertexArrayID = 0;
    fInstanceBuffer.reset();
    fDrawIndirectBuffer.reset();
    fInstanceAttribsBufferUniqueId = SK_InvalidUniqueID;
}

}
