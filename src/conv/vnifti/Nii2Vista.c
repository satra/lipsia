/*
** read nifti-1 to vista format
*/
#include <viaio/Vlib.h>
#include <viaio/VImage.h>
#include <viaio/mu.h>
#include <viaio/option.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>

#include <gsl/gsl_math.h>

#include <nifti/nifti1.h>
#include <nifti/nifti1_io.h>

#define MIN_HEADER_SIZE 348
#define NII_HEADER_SIZE 352

#define TINY 1.0e-10
#define ABS(x) ((x) > 0 ? (x) : -(x))

extern char *VReadGzippedData(char *filename,size_t *len);
extern char *VReadUnzippedData(char *filename,VBoolean nofail,size_t *size);
extern char *VReadDataContainer(char *filename,VBoolean nofail,size_t *size);
extern FILE *VReadInputFile (char *filename,VBoolean nofail);
extern int  CheckGzip(char *filename);

extern void VByteSwapNiftiHeader(nifti_1_header *hdr);
extern void VByteSwapData(char *data,size_t ndata,size_t nsize);


/* set dimension in geo info */
void VSetGeo3d4d(VAttrList geolist,int dimtype)
{
  double *D = VGetGeoDim(geolist,NULL);
  D[0] = (double)dimtype;
  VSetGeoDim(geolist,D);
} 


/*
 *  VIniImage
 *
 *  Allocate Image structure and fill voxel values with databuffer.
 *  Returns a pointer to the image if successful, zero otherwise.
 *  Useful for converting from 4D nifti to list of 3D vista images
 */
VImage VIniImage (int nbands, int nrows, int ncolumns, VRepnKind pixel_repn, char *databuffer)
{
  size_t row_size = ncolumns * VRepnSize (pixel_repn);
  size_t data_size = nbands * nrows * row_size;
  size_t row_index_size = nbands * nrows * sizeof (char *);
  size_t band_index_size = nbands * sizeof (char **);
  size_t pixel_size=0;
  VImage image;
  int band=0, row=0;
  char *p = NULL;

  /* Check parameters: */
  if (nbands < 1) VError("VIniImage: Invalid number of bands: %d", (int) nbands);
  if (nrows < 1)  VError ("VIniImage: Invalid number of rows: %d", (int) nrows);
  if (ncolumns < 1) VError ("VIniImage: Invalid number of columns: %d",(int) ncolumns);


#define AlignUp(v, b) ((((v) + (b) - 1) / (b)) * (b))

  /* Initialize VImage data struct */ 
  pixel_size = VRepnSize (pixel_repn);
  p = VMalloc(AlignUp (sizeof (VImageRec) + row_index_size + band_index_size, pixel_size));

  image = (VImage) p;
  image->nbands = nbands;
  image->nrows  = nrows;
  image->ncolumns = ncolumns;
  image->flags    = VImageSingleAlloc;
  image->pixel_repn = pixel_repn;
  image->attributes = VCreateAttrList ();
  image->band_index = (VPointer **) (p += sizeof (VImageRec));
  image->row_index  = (VPointer *) (p += band_index_size);
  image->data = (VPointer) AlignUp((long) (databuffer+4), pixel_size);

  image->nframes = nbands;
  image->nviewpoints = image->ncolors = image->ncomponents = 1;

  /* Initialize the indices: */
  for (band = 0; band < nbands; band++)
    image->band_index[band] = image->row_index + band * nrows;
  for (row = 0, p = image->data; row < nbands * nrows; row++, p += row_size)
    image->row_index[row] = p;

  return image;

#undef AlignUp
}


#define xgetval(data,index,Datatype) \
{ \
  Datatype *k = (Datatype *)(&data[index]); \
  u = (double)(*k); \
}


double VGetValue(char *data,size_t index,int datatype)
{
  double u=0;
  switch(datatype) {
  case DT_BINARY:
    xgetval(data,index,VBit);
    break;
  case DT_UNSIGNED_CHAR:
    xgetval(data,index,VUByte);
    break;
  case DT_SIGNED_SHORT:
    xgetval(data,index,short);
    break;
  case DT_SIGNED_INT:
    xgetval(data,index,int);
    break;
  case DT_FLOAT:
    xgetval(data,index,float);
    break;
  case DT_DOUBLE:
    xgetval(data,index,double);
    break;
  case DT_INT8:
    xgetval(data,index,VSByte);
    break;
  case DT_UINT16:
    xgetval(data,index,unsigned short);
    break;
  case DT_UINT32:
    xgetval(data,index,unsigned int);
    break;
  case DT_INT64:
    xgetval(data,index,long);
    break;
  case DT_UINT64:
    xgetval(data,index,unsigned long);
    break;
  default:
    VError(" unknown datatype %d",datatype);
  }
  return u;
}


/* get image statistics for re-scaling parameters */
void VDataStats(char *data,size_t ndata,size_t nsize,int datatype,double *xmin,double *xmax)
{
  size_t i,n;
  double zmin = VRepnMaxValue(VDoubleRepn);
  double zmax = VRepnMinValue(VDoubleRepn);

  n=0;
  for (i=0; i<ndata; i+= nsize) {
    double u = VGetValue(data,i,datatype);
    if (fabs(u) < TINY) continue;
    if (u < zmin) zmin = u;
    if (u > zmax) zmax = u;
    n++;
  }
  if (n < 1) VError(" no non-zero data points found");

  *xmin = (zmin+TINY);
  *xmax = (zmax-TINY);
}



/* list of 3D images */
void Nii2Vista3DList(char *data,size_t nsize,size_t nslices,size_t nrows,size_t ncols,size_t nt,
		     VRepnKind pixel_repn,VString voxelstr,VShort tr,VAttrList out_list)
{
  size_t i;
  size_t npix = nrows*ncols*nslices;

  VImage *dst = (VImage *) VCalloc(nt,sizeof(VImage));
  VAppendAttr(out_list,"nimages",NULL,VLongRepn,(VLong) nt);

  for (i=0; i<nt; i++) {
    const size_t index = i*npix*nsize;
    dst[i] = VIniImage(nslices,nrows,ncols,pixel_repn,&data[index]);
    VSetAttr(VImageAttrList(dst[i]),"voxel",NULL,VStringRepn,voxelstr);
    if (tr > 0) VSetAttr(VImageAttrList(dst[i]),"repetition_time",NULL,VShortRepn,tr);
    VAppendAttr(out_list,"image",NULL,VImageRepn,dst[i]);
  }
}



/* 4D time series data */
void Nii2Vista4D(char *data,size_t nsize,size_t nslices,size_t nrows,size_t ncols,size_t nt,
		 int datatype,double xmin,double xmax,
		 VString voxelstr,double *slicetime,VShort tr,VAttrList out_list)
{
  size_t slice,row,col,ti;
  size_t nrnc = nrows*ncols;
  size_t npix = nrnc*nslices;

  double umin = 0;
  double umax = VRepnMaxValue(VShortRepn);

  VImage *dst = (VImage *) VCalloc(nslices,sizeof(VImage));

  for (slice=0; slice<nslices; slice++) {
    dst[slice] = VCreateImage(nt,nrows,ncols,VShortRepn);
    if (!dst[slice]) VError(" err allocating image");
    VFillImage(dst[slice],VAllBands,0);

    VSetAttr(VImageAttrList(dst[slice]),"voxel",NULL,VStringRepn,voxelstr);
    if (tr > 0) VSetAttr(VImageAttrList(dst[slice]),"repetition_time",NULL,VShortRepn,tr);
    if (slicetime != NULL)
      VSetAttr(VImageAttrList(dst[slice]),"slice_time",NULL,VShortRepn,(VShort)slicetime[slice]);

    for (ti=0; ti<nt; ti++) {
      for (row=0; row<nrows; row++) {
	for (col=0; col<ncols; col++) {
	  const size_t index = (col + row*ncols + slice*nrnc + ti*npix + 1)*nsize;
	  /*
	  const size_t src_index = (col + row*ncols + i*nrnc + j*npix)*nsize;
	  const size_t dst_index = (col + row*ncols + j*nrnc)*nsize;
	  for (k=0; k<nsize; k++) {
	    pp[dst_index+k] = data[src_index+k];
	  }
	  */
	  
	  double u = VGetValue(data,index,datatype);
	  if (fabs(u) > TINY) {
	    u = umax * (u-xmin)/(xmax-xmin);
	  }
	  if (u < umin) u = umin;
	  if (u > umax) u = umax;
	  VPixel(dst[slice],ti,row,col,VShort) = (VShort)u;
	  
	}
      }
    }
    VAppendAttr(out_list,"image",NULL,VImageRepn,dst[slice]);
  }
}



/* copy nifti header infos to geolist in vista header */
double *VGetNiftiHeader(VAttrList geolist,nifti_1_header hdr,VLong tr)
{

  /* units in lipsia: mm and sec */
  char xyzt = hdr.xyzt_units;
  int spaceunits = XYZT_TO_SPACE(xyzt);
  int timeunits  = XYZT_TO_TIME(xyzt);
  double xscale  = 1.0;
  double tscale  = 1.0;
  if (spaceunits == NIFTI_UNITS_MICRON) xscale=1000.0;
  if (timeunits == NIFTI_UNITS_SEC) tscale=1000.0;

 
  /* dim info */
  VSetAttr(geolist,"dim_info",NULL,VShortRepn,(VShort)hdr.dim_info);


  /* dim */
  /* hdr.dim[0]==3 means dim=3,  hdr.dim[0]==4 means dim=4 (timeseries) */
  float *E = VCalloc(8,sizeof(float));
  VAttrList elist = VCreateAttrList();
  VBundle ebundle = VCreateBundle ("bundle",elist,8*sizeof(float),(VPointer)E);
  VSetAttr(geolist,"dim",NULL,VBundleRepn,ebundle);
  int i;
  for (i=0; i<8; i++) E[i] = hdr.dim[i];
  for (i=5; i<8; i++) E[i] = 0;



  /* pixdim */  
  float *D = VCalloc(8,sizeof(float));
  for (i=0; i<8; i++) D[i] = hdr.pixdim[i];
  for (i=1; i<=3; i++) D[i] *= xscale; 
  D[4] *= tscale;
  if (tr > 0) D[4] = (double)tr;
  for (i=5; i<8; i++) D[i] = 0; 
  VAttrList dlist = VCreateAttrList();
  VBundle dbundle = VCreateBundle ("bundle",dlist,8*sizeof(float),(VPointer)D);
  VSetAttr(geolist,"pixdim",NULL,VBundleRepn,dbundle);



  /* qform */
  size_t dim=6;
  float *Q = VCalloc(dim,sizeof(float));
  VAttrList qlist = VCreateAttrList();
  VBundle qbundle = VCreateBundle ("bundle",qlist,dim*sizeof(float),(VPointer)Q);
  Q[0] = hdr.quatern_b;
  Q[1] = hdr.quatern_c;
  Q[2] = hdr.quatern_d;
  Q[3] = hdr.qoffset_x;
  Q[4] = hdr.qoffset_y;
  Q[5] = hdr.qoffset_z;
  VSetAttr(geolist,"qform_code",NULL,VShortRepn,(VShort)hdr.qform_code);
  VSetAttr(geolist,"qform",NULL,VBundleRepn,qbundle);



  /* sform */
  VImage sform = VCreateImage(1,4,4,VFloatRepn);
  VFillImage (sform,VAllBands,0);
  int j;
  for (j=0; j<4; j++) {
    VPixel(sform,0,0,j,VFloat) = hdr.srow_x[j];
    VPixel(sform,0,1,j,VFloat) = hdr.srow_y[j];
    VPixel(sform,0,2,j,VFloat) = hdr.srow_z[j];
  }
  VSetAttr(geolist,"sform_code",NULL,VShortRepn,(VShort)hdr.sform_code);
  VSetAttr(geolist,"sform",NULL,VImageRepn,sform);


  /* MRI encoding directions */
  int freq_dim=0,phase_dim=0,slice_dim=0;
  if (hdr.dim_info != 0) {
    freq_dim  = DIM_INFO_TO_FREQ_DIM (hdr.dim_info );
    phase_dim = DIM_INFO_TO_PHASE_DIM(hdr.dim_info );
    slice_dim = DIM_INFO_TO_SLICE_DIM(hdr.dim_info );
    VSetAttr(geolist,"freq_dim",NULL,VShortRepn,(VShort)freq_dim);
    VSetAttr(geolist,"phase_dim",NULL,VShortRepn,(VShort)phase_dim);
    VSetAttr(geolist,"slice_dim",NULL,VShortRepn,(VShort)slice_dim);
  }
  if (slice_dim == 0) return NULL;

  /* slicetiming information */
  int slice_start = hdr.slice_start;
  int slice_end = hdr.slice_end;
  int slice_code = hdr.slice_code;
  double slice_duration = hdr.slice_duration;
  if (slice_code == 0 || slice_duration < 1.0e-10) return NULL;
  fprintf(stderr," slice duration: %.2f ms\n", slice_duration);
  VSetAttr(geolist,"slice_start",NULL,VShortRepn,(VShort)slice_start);
  VSetAttr(geolist,"slice_end",NULL,VShortRepn,(VShort)slice_end);
  VSetAttr(geolist,"slice_code",NULL,VShortRepn,(VShort)slice_code);
  VSetAttr(geolist,"slice_duration",NULL,VFloatRepn,(VFloat)slice_duration);

  int nslices = (int)E[3];
  double *slicetimes = (double *) VCalloc(nslices,sizeof(double));
  VGetSlicetimes(slice_start,slice_end,slice_code,slice_duration,slicetimes);
  return slicetimes;
}



VAttrList Nifti1_to_Vista(char *databuffer,VLong tr,VBoolean attrtype)
{

  /* read header */
  nifti_1_header hdr;
  memcpy(&hdr,databuffer,MIN_HEADER_SIZE);
  if ((strncmp(hdr.magic,"ni1\0",4) != 0) && (strncmp(hdr.magic,"n+1\0",4) != 0))
    VError(" not a nifti-1 file, magic number is %s",hdr.magic);


  int swap = NIFTI_NEEDS_SWAP(hdr);
  if (swap == 1) {
    VByteSwapNiftiHeader(&hdr);
  }

  /* get data type */
  VRepnKind dst_repn;
  int datatype = (int)hdr.datatype;

  switch(datatype) {
  case DT_UNKNOWN:
    VError(" unknown data type");
    break;
  case DT_BINARY:
    dst_repn = VBitRepn;
    break;
  case DT_UNSIGNED_CHAR:
    dst_repn = VUByteRepn;
    break;
  case DT_SIGNED_SHORT:
    dst_repn = VShortRepn;
    break;
  case DT_SIGNED_INT:
    dst_repn = VIntegerRepn;
    break;
  case DT_FLOAT:
    dst_repn = VFloatRepn;
    break;
  case DT_DOUBLE:
    dst_repn = VDoubleRepn;
    break;
  case DT_INT8:
    dst_repn = VSByteRepn;
    break;
  case DT_UINT16:
    dst_repn = VUShortRepn;
    break;
  case DT_UINT32:
    dst_repn = VUIntegerRepn;
    break;
  case DT_INT64:
    dst_repn = VLongRepn;
    break;
  case DT_UINT64:
    dst_repn = VULongRepn;
    break;
  default:
    VError(" unknown data type %d",datatype);
  }


  /* number of values stored at each time point */
  if (hdr.dim[5] > 1) VError("data type not supported, dim[5]= %d\n",hdr.dim[5]);


  /* image size */
  size_t dimtype = (size_t)hdr.dim[0];
  size_t ncols   = (size_t)hdr.dim[1];
  size_t nrows   = (size_t)hdr.dim[2];
  size_t nslices = (size_t)hdr.dim[3];
  size_t nt      = (size_t)hdr.dim[4];


  /* fill data container */
  size_t bytesize = 8;
  if (dst_repn == VBitRepn) bytesize = 1;
  size_t nsize   = hdr.bitpix/bytesize;
  size_t npixels = nslices * nrows * ncols;
  size_t ndata   = nt * npixels * nsize;
  size_t hdrsize = MIN_HEADER_SIZE;
  char *data = &databuffer[hdrsize];
  


  /* byte swap image data, if needed */
  if (swap == 1) {
    VByteSwapData(data,ndata,nsize);
  }

  /* functional data must be VShortRepn, rescale if needed */
  double xmin=0,xmax=0;
  if (nt > 1 || dimtype == 4) {
    /* fprintf(stderr," scaling...\n"); */
    size_t npixels = nslices * nrows * ncols;
    size_t ndata   = nt * npixels * nsize;
    VDataStats(data,ndata,nsize,datatype,&xmin,&xmax);
    fprintf(stderr," data range: [%f, %f]\n",xmin,xmax);
  }


  /* repetition time (may be wrong in some cases) */
  char xyzt = hdr.xyzt_units;
  int tcode = XYZT_TO_TIME(xyzt);
  float factor = 1.0;
  if (tcode == NIFTI_UNITS_MSEC) factor = 1.0;
  if (tcode == NIFTI_UNITS_SEC) factor = 1000.0;
  if (nt > 1)  {
    if (tr == 0) tr = (short)(factor*hdr.pixdim[4]);
    fprintf(stderr," nt=%ld,  TR= %ld\n",nt,tr);
    if (tr < 1) VWarning(" implausible TR (%d ms), use parameter '-tr' to set correct TR",tr);
  }

  
  /* voxel reso */
  int blen=512;
  char *voxelstr = (char *) VCalloc(blen,sizeof(char));
  memset(voxelstr,0,blen);
  sprintf(voxelstr,"%f %f %f",hdr.pixdim[1],hdr.pixdim[2],hdr.pixdim[3]);
  fprintf(stderr," voxel: %.4f %.4f %.4f\n",hdr.pixdim[1],hdr.pixdim[2],hdr.pixdim[3]);


  /* geometry information */
  VAttrList geolist = VCreateAttrList();
  double *slicetime = VGetNiftiHeader(geolist,hdr,tr);
  

  /* read nii image into vista attrlist */
  VAttrList out_list = VCreateAttrList();
  VAppendAttr(out_list,"geoinfo",NULL,VAttrListRepn,geolist);


  if (nt <= 1) {            /* output one 3D image */
    Nii2Vista3DList(data,nsize,nslices,nrows,ncols,nt,dst_repn,voxelstr,(VShort)tr,out_list);
    VSetGeo3d4d(geolist,(int) 3);
  }
  else if (attrtype == FALSE) {  /* output list of 3D images */
    Nii2Vista3DList(data,nsize,nslices,nrows,ncols,nt,dst_repn,voxelstr,(VShort)tr,out_list);
    VSetGeo3d4d(geolist,(int) 3);
  }
  else if (attrtype == TRUE && nt > 1) {   /* output one 4D image */
    Nii2Vista4D(data,nsize,nslices,nrows,ncols,nt,datatype,xmin,xmax,voxelstr,slicetime,(VShort)tr,out_list);
    VSetGeo3d4d(geolist,(int) 4);
  }

  return out_list;
}
