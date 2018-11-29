#ifndef GFF_H
#define GFF_H

//#define CUFFLINKS 1

#include "GBase.h"
#include "gdna.h"
#include "codons.h"
#include "GFaSeqGet.h"
#include "GList.hh"
#include "GHash.hh"

#ifdef CUFFLINKS
#include <boost/crc.hpp>  // for boost::crc_32_type
#endif

//reserved Gffnames::feats entries -- basic feature types
extern int gff_fid_mRNA; // "mRNA" feature name
extern int gff_fid_transcript; // *RNA, *transcript feature name
extern int gff_fid_exon;

extern const uint GFF_MAX_LOCUS;
extern const uint GFF_MAX_EXON;
extern const uint GFF_MAX_INTRON;

extern const uint gfo_flag_CHILDREN_PROMOTED;
extern const uint gfo_flag_HAS_ERRORS;
extern const uint gfo_flag_IS_GENE;
extern const uint gfo_flag_HAS_GFF_ID; //has a ID (GFF3) or transcript_id (GTF)
extern const uint gfo_flag_BY_EXON;  //created by subfeature (exon) directly
                      //(GTF2 and some chado gff3 dumps with exons given before their mRNA)
extern const uint gfo_flag_IS_TRANSCRIPT; //recognized as '*RNA' or '*transcript'
extern const uint gfo_flag_DISCARDED; //should not be printed under the "transcriptsOnly" directive
extern const uint gfo_flag_LST_KEEP; //GffObj from GffReader::gflst is to be kept (not deallocated)
                                     //when GffReader is destroyed
extern const uint gfo_flag_LEVEL_MSK; //hierarchical level: 0 = no parent
extern const byte gfo_flagShift_LEVEL;

extern bool gff_show_warnings;

#define GFF_LINELEN 4096
#define ERR_NULL_GFNAMES "Error: GffObj::%s requires a non-null GffNames* names!\n"


enum GffExonType {
  exgffIntron=-1, // useless "intron" feature
	exgffNone=0,  //not a recognizable exonic segment
  exgffStart, //from "start_codon" feature (within CDS)
  exgffStop, //from "stop_codon" feature (may be outside CDS)
  exgffCDS,  //from "CDS" feature
  exgffUTR,  //from "UTR" feature
  exgffCDSUTR, //from a merge of UTR and CDS feature
  exgffExon, //from "exon" feature
};

const char* strExonType(char xtype);

class GffReader;
class GffObj;

//---transcript overlapping - utility functions:
int classcode_rank(char c); //returns priority value for class codes

char getOvlCode(GffObj& m, GffObj& r, int& ovlen); //returns: class code

bool singleExonTMatch(GffObj& m, GffObj& r, int& ovlen); //single-exon fuzzy transcript match
//---

//reading a whole transcript from a BED-12 line
class BEDLine {
 public:
    bool skip;
    char* dupline; //duplicate of original line
    char* line; //this will have tabs replaced by \0
    int llen;
    char* gseqname;
    uint fstart;
    uint fend;
    char strand;
    char* ID; //transcript ID from BED-12+ (13th if exists, otherwise 4th)
    char* info; //13th column - these could be GFF3 attributes
    uint cds_start;
    uint cds_end;
    char cds_phase;
    GVec<GSeg> exons;
    BEDLine(GffReader* r=NULL, const char* l=NULL);
    ~BEDLine() {
    	GFREE(dupline);
    	GFREE(line);
    	//GFREE(ID);
    	//GFREE(extra);
    }
};

class GffLine {
    char* _parents; //stores a copy of the Parent attribute value,
       //with commas replaced by \0
    int _parents_len;
 public:
    char* dupline; //duplicate of original line
    char* line; //this will have tabs replaced by \0
    int llen;
    char* gseqname;
    char* track;
    char* ftype; //feature name: mRNA/gene/exon/CDS
    int ftype_id;
    char* info; //the last, attributes' field, unparsed
    uint fstart;
    uint fend;
    uint qstart; //overlap coords on query, if available
    uint qend;
    uint qlen; //query len, if given
    double score;
    char strand;
    union {
    	uint8_t flags;
    	struct {
    		bool is_exonlike:2;
    	};
    	struct {
    	    bool is_cds:1; //"cds" or "start/stop_codon" features
    	    bool is_exon:1; //"exon" and "utr" features
    	    bool is_transcript:1; //if current feature is *RNA or *transcript
    	    bool is_gene:1; //current feature is *gene
    	    //bool is_gff3:1; //line appears to be in GFF3 format (0=GTF)
    	    bool is_gtf_transcript:1; //GTF transcript line with Parents parsed from gene_id
    	    bool skipLine:1;
    	};
    };
    char exontype; // gffExonType
    char phase;  // '.' , '0', '1' or '2', can be also given as CDSphase attribute in TLF
    uint cds_start; //if TLF: CDS attribute
    uint cds_end;
    GVec<GSeg> exons; //if TLF: exons= attribute
    // -- allocated strings:
    char* gene_name; //value of gene_name attribute (GTF) if present or Name attribute of a gene feature (GFF3)
    char* gene_id; //value of gene_id attribute (GTF) if present or ID attribute of a gene feature (GFF3)
    //
    char** parents; //for GTF only parents[0] is used
    int num_parents;
    char* ID;     // if a ID=.. attribute was parsed, or a GTF with 'transcript' line (transcript_id)
    GffLine(GffReader* reader, const char* l); //parse the line accordingly
    void discardParent() {
    	GFREE(_parents);
    	_parents_len=0;
    	num_parents=0;
    	GFREE(parents);
    	parents=NULL;
    }
    static char* extractGFFAttr(char*& infostr, const char* oline, const char* pre, bool caseStrict=false, bool enforce_GTF2=false, int* rlen=NULL);
    char* extractAttr(const char* pre, bool caseStrict=false, bool enforce_GTF2=false, int* rlen=NULL){
    	return extractGFFAttr(info, dupline, pre, caseStrict, enforce_GTF2, rlen);
    }
    GffLine(GffLine& l): _parents(NULL), _parents_len(l._parents_len),
    		dupline(NULL), line(NULL), llen(l.llen), gseqname(NULL), track(NULL),
    		ftype(NULL), ftype_id(l.ftype_id), info(NULL), fstart(l.fstart), fend(l.fend), qstart(l.fstart), qend(l.fend),
			qlen(l.qlen), score(l.score), strand(l.strand), flags(l.flags), exontype(l.exontype),
			phase(l.phase), cds_start(l.cds_start), cds_end(l.cds_end), exons(l.exons),
			gene_name(NULL), gene_id(NULL), parents(NULL), num_parents(l.num_parents), ID(NULL) {
    	//if (l==NULL || l->line==NULL)
    	//	GError("Error: invalid GffLine(l)\n");
    	//memcpy((void*)this, (void*)l, sizeof(GffLine));
    	GMALLOC(line, llen+1);
    	memcpy(line, l.line, llen+1);
    	GMALLOC(dupline, llen+1);
    	memcpy(dupline, l.dupline, llen+1);
    	//--offsets within line[]
    	gseqname=line+(l.gseqname-l.line);
    	track=line+(l.track-l.line);
    	ftype=line+(l.ftype-l.line);
    	info=line+(l.info-l.line);
    	if (num_parents>0 && parents) {
    		GMALLOC(parents, num_parents*sizeof(char*));
    		//_parents_len=l->_parents_len; copied above
    		_parents=NULL; //re-init, forget pointer copy
    		GMALLOC(_parents, _parents_len);
    		memcpy(_parents, l._parents, _parents_len);
    		for (int i=0;i<num_parents;i++) {
    			parents[i]=_parents+(l.parents[i] - l._parents);
    		}
    	}
    	//-- allocated string copies:
    	ID=Gstrdup(l.ID);
    	if (l.gene_name!=NULL)
    		gene_name=Gstrdup(l.gene_name);
    	if (l.gene_id!=NULL)
    		gene_id=Gstrdup(l.gene_id);
    }
    GffLine(): _parents(NULL), _parents_len(0),
    		dupline(NULL), line(NULL), llen(0), gseqname(NULL), track(NULL),
    		ftype(NULL), ftype_id(-1), info(NULL), fstart(0), fend(0), qstart(0), qend(0), qlen(0),
    		score(0), strand(0), flags(0), exontype(0), phase(0), cds_start(0), cds_end(0), exons(),
    		gene_name(NULL), gene_id(NULL), parents(NULL), num_parents(0), ID(NULL) {
    }
    ~GffLine() {
    	GFREE(dupline);
    	GFREE(line);
    	GFREE(_parents);
    	GFREE(parents);
    	GFREE(ID);
    	GFREE(gene_name);
    	GFREE(gene_id);
    }
};

class GffAttr {
 public:
   int attr_id;
   char* attr_val;
   GffAttr(int an_id, const char* av=NULL) {
     attr_id=an_id;
     attr_val=NULL;
     setValue(av);
     }
  ~GffAttr() {
     GFREE(attr_val);
     }
  void setValue(const char* av) {
     if (attr_val!=NULL) {
        GFREE(attr_val);
        }
     if (av==NULL || av[0]==0) return;
     //trim spaces
     const char* vstart=av;
     while (*vstart==' ') av++;
     const char* vend=vstart;
     bool keep_dq=false;
     while (vend[1]!=0) {
        if (*vend==' ' && vend[1]!=' ') keep_dq=true;
          else if (*vend==';') keep_dq=true;
        vend++;
        }
     //remove spaces at the end:
     while (*vend==' ' && vend!=vstart) vend--;
     //practical clean-up: if it doesn't have any internal spaces just strip those useless double quotes
     if (!keep_dq && *vstart=='"' && *vend=='"') {
               vend--;
               vstart++;
               }
     attr_val=Gstrdup(vstart, vend);
     }
  bool operator==(GffAttr& d){
      return (this==&d);
      }
  bool operator>(GffAttr& d){
     return (this>&d);
     }
  bool operator<(GffAttr& d){
    return (this<&d);
    }

 };

class GffNameList;
class GffNames;

class GffNameInfo {
  friend class GffNameList;
 public:
   int idx;
   char* name;
   GffNameInfo(const char* n=NULL):idx(-1),name(NULL) {
     if (n) name=Gstrdup(n);
     }

   ~GffNameInfo() {
      GFREE(name);
     }

   bool operator==(GffNameInfo& d){
       return (strcmp(this->name, d.name)==0);
       }
   bool operator<(GffNameInfo& d){
     return (strcmp(this->name, d.name)<0);
     }
};

class GffNameList:public GPVec<GffNameInfo> {
  friend class GffNameInfo;
  friend class GffNames;
protected:
  GHash<GffNameInfo> byName;//hash with shared keys
  int idlast; //fList index of last added/reused name
  int addStatic(const char* tname) {// fast add
     GffNameInfo* f=new GffNameInfo(tname);
     idlast=this->Add(f);
     f->idx=idlast;
     byName.shkAdd(f->name,f);
     return idlast;
     }
public:
 //GffNameList(int init_capacity=6):GList<GffNameInfo>(init_capacity, false,true,true), byName(false) {
  GffNameList(int init_capacity=6):GPVec<GffNameInfo>(init_capacity, true), byName(false) {
    idlast=-1;
    setCapacity(init_capacity);
    }
 char* lastNameUsed() { return idlast<0 ? NULL : Get(idlast)->name; }
 int lastNameId() { return idlast; }
 char* getName(int nid) { //retrieve name by its ID
   if (nid<0 || nid>=fCount)
         GError("GffNameList Error: invalid index (%d)\n",nid);
   return fList[nid]->name;
   }

 int addName(const char* tname) {//returns or create an id for the given name
   //check idlast first, chances are it's the same feature name checked
   /*if (idlast>=0 && strcmp(fList[idlast]->name,tname)==0)
       return idlast;*/
   GffNameInfo* f=byName.Find(tname);
   int fidx=-1;
   if (f!=NULL) fidx=f->idx;
     else {//add new entry
      f=new GffNameInfo(tname);
      fidx=this->Add(f);
      f->idx=fidx;
      byName.shkAdd(f->name,f);
      }
   idlast=fidx;
   return fidx;
   }

 int addNewName(const char* tname) {
    GffNameInfo* f=new GffNameInfo(tname);
    int fidx=this->Add(f);
    f->idx=fidx;
    byName.shkAdd(f->name,f);
    return fidx;
    }

 int getId(const char* tname) { //only returns a name id# if found
    GffNameInfo* f=byName.Find(tname);
    if (f==NULL) return -1;
    return f->idx;
    }
 int removeName() {
   GError("Error: removing names from GffNameList not allowed!\n");
   return -1;
   }
};

class GffNames {
 public:
   int numrefs;
   GffNameList tracks;
   GffNameList gseqs;
   GffNameList attrs;
   GffNameList feats; //feature names: 'mRNA', 'exon', 'CDS' etc.
   GffNames():tracks(),gseqs(),attrs(), feats() {
    numrefs=0;
    //the order below is critical!
    //has to match: gff_fid_mRNA, gff_fid_exon
    gff_fid_mRNA = feats.addStatic("mRNA");//index 0=gff_fid_mRNA
    gff_fid_transcript=feats.addStatic("transcript");//index 1=gff_fid_transcript
    gff_fid_exon=feats.addStatic("exon");//index 1=gff_fid_exon
    //feats.addStatic("CDS"); //index 2=gff_fid_CDS
    }
};

void gffnames_ref(GffNames* &n);
void gffnames_unref(GffNames* &n);

enum GffPrintMode {
  pgtfAny, //print record as read, if GTF
  pgtfExon, //print exon only features
  pgtfCDS,  //print CDS and exon features
  pgffAny, //print record as read
  pgffExon,
  pgffCDS,
  pgffBoth,
  pgffTLF,  //exon and CDS data shown as additional GFF attributes
            //in the transcript line (Transcript Line Format)
            //every line has the whole transcript data
  pgffBED //print a BED line with all other GFF attributes in column 13
};


class GffAttrs:public GList<GffAttr> {
  public:
    GffAttrs():GList<GffAttr>(false,true,true) { }
    void add_or_update(GffNames* names, const char* attrname, const char* val) {
      int aid=names->attrs.getId(attrname);
      if (aid>=0) {
         //attribute found in the dictionary
         for (int i=0;i<Count();i++) {
            //do we have it?
            if (aid==Get(i)->attr_id) {
                //update the value
                Get(i)->setValue(val);
                return;
                }
            }
         }
        else {
         aid=names->attrs.addNewName(attrname);
         }
      this->Add(new GffAttr(aid, val));
      }

    char* getAttr(GffNames* names, const char* attrname) {
      int aid=names->attrs.getId(attrname);
      if (aid>=0)
        for (int i=0;i<Count();i++)
          if (aid==Get(i)->attr_id) return Get(i)->attr_val;
      return NULL;
      }
    char* getAttr(int aid) {
      if (aid>=0)
        for (int i=0;i<Count();i++)
          if (aid==Get(i)->attr_id) return Get(i)->attr_val;
      return NULL;
      }
};


class GffExon : public GSeg {
 public:
  void* uptr; //for later extensions
  GffAttrs* attrs; //other attributes kept for this exon
  double score; // gff score column
  char phase; //GFF phase column - for CDS segments only
             // '.' = undefined (UTR), '0','1','2' for CDS exons
  char exontype; // 1="exon" 2="cds" 3="utr" 4="stop_codon"
  int qstart; // for mRNA/protein exon mappings: coordinates on query
  int qend;
  GffExon(int s=0, int e=0, double sc=0, char fr=0, int qs=0, int qe=0, char et=0) {
    uptr=NULL;
    attrs=NULL;
    if (s<e) {
      start=s;
      end=e;
      }
   else {
     start=e;
     end=s;
    }
   if (qs<qe) {
     qstart=qs;
     qend=qe;
     } else {
     qstart=qe;
     qend=qs;
     }
   score=sc;
   phase=fr;
   exontype=et;
   } //constructor

 char* getAttr(GffNames* names, const char* atrname) {
   if (attrs==NULL || names==NULL || atrname==NULL) return NULL;
   return attrs->getAttr(names, atrname);
   }

 char* getAttr(int aid) {
   if (attrs==NULL) return NULL;
   return attrs->getAttr(aid);
   }

 ~GffExon() { //destructor
   if (attrs!=NULL) delete attrs;
   }
};


class GffCDSeg:public GSeg {
 public:
  char phase;
  int exonidx;
};

//one GFF mRNA object -- e.g. a mRNA with its exons and/or CDS segments
class GffObj:public GSeg {
  //utility segment-merging function for addExon()
  void expandExon(int xovl, uint segstart, uint segend,
       char exontype, double sc, char fr, int qs, int qe);
 protected:
   //coordinate transformation data:
   uint xstart; //absolute genomic coordinates of reference region
   uint xend;
   char xstatus; //coordinate transform status:
            //0 : (start,end) coordinates are absolute
            //'+' : (start,end) coords are relative to xstart..xend region
            //'-' : (start,end) are relative to the reverse complement of xstart..xend region
   //--
   char* gffID; // ID name for mRNA (parent) feature
   char* gene_name; //value of gene_name attribute (GTF) if present or Name attribute of the parent gene feature (GFF3)
   char* geneID; //value of gene_id attribute (GTF) if present, or the ID attribute of a parent gene feature (GFF3)
   unsigned int flags;
   //-- friends:
   friend class GffReader;
   friend class GffExon;
public:
  static GffNames* names; // dictionary storage that holds the various attribute names etc.
  //TODO: this makes the parser thread unsafe, use conditional compiling for names access!
  int track_id; // index of track name in names->tracks
  int gseq_id; // index of genomic sequence name in names->gseqs
  int ftype_id; // index of this record's feature name in names->feats, or the special gff_fid_mRNA value
  int exon_ftype_id; //index of child subfeature name in names->feats (that subfeature stored in "exons")
                   //if ftype_id==gff_fid_mRNA then this value is ignored
  GList<GffExon> exons; //for non-mRNA entries, these can be any subfeature of type subftype_id
  GPVec<GffObj> children;
  GffObj* parent;
  int udata; //user data, flags etc.
  void* uptr; //user pointer (to a parent object, cluster, locus etc.)
  GffObj* ulink; //link to another GffObj (user controlled field)
  // mRNA specific fields:
  bool isCDS; //just a CDS, no UTRs
  bool partial; //partial CDS
  uint CDstart; //CDS start coord
  uint CDend;   //CDS end coord
  char CDphase; //initial phase for CDS start ('.','0'..'2')
                //CDphase is at CDend if strand=='-'
  static void decodeHexChars(char* dbuf, const char* s, int maxlen=1023);
  bool hasErrors() { return ((flags & gfo_flag_HAS_ERRORS)!=0); }
  void hasErrors(bool v) {
      if (v) flags |= gfo_flag_HAS_ERRORS;
        else flags &= ~gfo_flag_HAS_ERRORS;
      }
  bool hasGffID() { return ((flags & gfo_flag_HAS_GFF_ID)!=0); }
  void hasGffID(bool v) {
      if (v) flags |= gfo_flag_HAS_GFF_ID;
        else flags &= ~gfo_flag_HAS_GFF_ID;
      }
  bool createdByExon() { return ((flags & gfo_flag_BY_EXON)!=0); }
  void createdByExon(bool v) {
      if (v) flags |= gfo_flag_BY_EXON;
        else flags &= ~gfo_flag_BY_EXON;
      }
  bool isGene() { return ((flags & gfo_flag_IS_GENE)!=0); }
  void isGene(bool v) {
      if (v) flags |= gfo_flag_IS_GENE;
        else flags &= ~gfo_flag_IS_GENE;
      }
  bool isDiscarded() { return ((flags & gfo_flag_DISCARDED)!=0); }
  void isDiscarded(bool v) {
      if (v) flags |= gfo_flag_DISCARDED;
        else flags &= ~gfo_flag_DISCARDED;
      }

  bool isUsed() { return ((flags & gfo_flag_LST_KEEP)!=0); }
  void isUsed(bool v) {
      if (v) flags |= gfo_flag_LST_KEEP;
        else flags &= ~gfo_flag_LST_KEEP;
      }
  bool isTranscript() { return ((flags & gfo_flag_IS_TRANSCRIPT)!=0); }
  void isTranscript(bool v) {
      if (v) flags |= gfo_flag_IS_TRANSCRIPT;
        else flags &= ~gfo_flag_IS_TRANSCRIPT;
      }
  bool promotedChildren() { return ((flags & gfo_flag_CHILDREN_PROMOTED)!=0); }
  void promotedChildren(bool v) {
    if (v) flags |= gfo_flag_CHILDREN_PROMOTED;
      else flags &= ~gfo_flag_CHILDREN_PROMOTED;
     }
  void setLevel(byte v) {
    if (v==0) flags &= ~gfo_flag_LEVEL_MSK;
         else flags &= ~(((uint)v) << gfo_flagShift_LEVEL);
    }
  byte incLevel() {
    uint v=((flags & gfo_flag_LEVEL_MSK) >> gfo_flagShift_LEVEL);
    v++;
    flags &= ~(v << gfo_flagShift_LEVEL);
    return v;
    }
  byte getLevel() {
    return ((byte)((flags & gfo_flag_LEVEL_MSK) >> gfo_flagShift_LEVEL));
    }

  bool isValidTranscript() {
    //return (ftype_id==gff_fid_mRNA && exons.Count()>0);
    return (isTranscript() && exons.Count()>0);
    }


  int addExon(uint segstart, uint segend, double sc=0, char fr='.',
             int qs=0, int qe=0, bool iscds=false, char exontype=0);

  int addExon(GffReader* reader, GffLine* gl, bool keepAttr=false, bool noExonAttr=true);

  void removeExon(int idx);
  void removeExon(GffExon* p);
  char  strand; //true if features are on the reverse complement strand
  double gscore;
  double uscore; //custom, user-computed score, if needed
  int covlen; //total coverage of  reference genomic sequence (sum of maxcf segment lengths)

   //--------- optional data:
  int qlen; //query length, start, end - if available
  int qstart;
  int qend;
  int qcov; //query coverage - percent
  GffAttrs* attrs; //other gff3 attributes found for the main mRNA feature
   //constructor by gff line parsing:
  GffObj(GffReader* gfrd, BEDLine* bedline, bool keepAttr=false);
  GffObj(GffReader* gfrd, GffLine* gffline, bool keepAttrs=false, bool noExonAttr=true);
   //if gfline->Parent!=NULL then this will also add the first sub-feature
   // otherwise, only the main feature is created
  void copyAttrs(GffObj* from);
  void clearAttrs() {
    if (attrs!=NULL) {
      bool sharedattrs=(exons.Count()>0 && exons[0]->attrs==attrs);
      delete attrs; attrs=NULL;
      if (sharedattrs) exons[0]->attrs=NULL;
      }
    }
  GffObj(char* anid=NULL):GSeg(0,0), exons(true,true,false), children(1,false) {
                                   //exons: sorted, free, non-unique
       gffID=NULL;
       uptr=NULL;
       ulink=NULL;
       flags=0;
       udata=0;
       parent=NULL;
       ftype_id=-1;
       exon_ftype_id=-1;
       if (anid!=NULL) gffID=Gstrdup(anid);
       gffnames_ref(names);
       qlen=0;
       qstart=0;
       qend=0;
       qcov=0;
       partial=true;
       isCDS=false;
       CDstart=0; // hasCDS <=> CDstart>0
       CDend=0;
       CDphase=0;
       gseq_id=-1;
       track_id=-1;
       xstart=0;
       xend=0;
       xstatus=0;
       strand='.';
       gscore=0;
       uscore=0;
       attrs=NULL;
       covlen=0;
       gene_name=NULL;
       geneID=NULL;
       }
   ~GffObj() {
       GFREE(gffID);
       GFREE(gene_name);
       GFREE(geneID);
       clearAttrs();
       gffnames_unref(names);
       }
   //--------------
   GffObj* finalize(GffReader* gfr, bool mergeCloseExons=false,
               bool keepAttrs=false, bool noExonAttr=true);
               //complete parsing: must be called in order to merge adjacent/close proximity subfeatures
   void parseAttrs(GffAttrs*& atrlist, char* info, bool isExon=false);
   const char* getSubfName() { //returns the generic feature type of the entries in exons array
     //int sid=exon_ftype_id;
     //if (sid==gff_fid_exon && isCDS) sid=gff_fid_CDS;
     return names->feats.getName(exon_ftype_id);
     }
   void addCDS(uint cd_start, uint cd_end, char phase=0);

   bool monoFeature() {
     return (exons.Count()==0 ||
          (exons.Count()==1 &&  //exon_ftype_id==ftype_id &&
              exons[0]->end==this->end && exons[0]->start==this->start));
     }

   bool hasCDS() { return (CDstart>0); }

   const char* getFeatureName() {
     return names->feats.getName(ftype_id);
     }
   void setFeatureName(const char* feature);

   void addAttr(const char* attrname, const char* attrvalue);
   int removeAttr(const char* attrname, const char* attrval=NULL);
   int removeAttr(int aid, const char* attrval=NULL);
   int removeExonAttr(GffExon& exon, const char* attrname, const char* attrval=NULL);
   int removeExonAttr(GffExon& exon, int aid, const char* attrval=NULL);
   const char* getAttrName(int i) {
     if (attrs==NULL) return NULL;
     return names->attrs.getName(attrs->Get(i)->attr_id);
     }
   char* getAttr(const char* attrname, bool checkFirstExon=false) {
     if (names==NULL || attrname==NULL) return NULL;
     char* r=NULL;
     if (attrs==NULL) {
         if (!checkFirstExon) return NULL;
         }
       else r=attrs->getAttr(names, attrname);
     if (r!=NULL) return r;
     if (checkFirstExon && exons.Count()>0) {
        r=exons[0]->getAttr(names, attrname);
        }
     return r;
     }

   char* getExonAttr(GffExon* exon, const char* attrname) {
      if (exon==NULL || attrname==NULL) return NULL;
      return exon->getAttr(names, attrname);
      }

   char* getExonAttr(int exonidx, const char* attrname) {
      if (exonidx<0 || exonidx>=exons.Count() || attrname==NULL) return NULL;
      return exons[exonidx]->getAttr(names, attrname);
      }

   char* getAttrValue(int i) {
     if (attrs==NULL) return NULL;
     return attrs->Get(i)->attr_val;
     }
   const char* getGSeqName() {
     return names->gseqs.getName(gseq_id);
     }

   const char* getRefName() {
     return names->gseqs.getName(gseq_id);
     }
   void setRefName(const char* newname);

   const char* getTrackName() {
     return names->tracks.getName(track_id);
     }
   bool exonOverlap(uint s, uint e) {//check if ANY exon overlaps given segment
      //ignores strand!
      if (s>e) Gswap(s,e);
      for (int i=0;i<exons.Count();i++) {
         if (exons[i]->overlap(s,e)) return true;
         }
      return false;
      }
    bool exonOverlap(GffObj& m) {//check if ANY exon overlaps given segment
      //if (gseq_id!=m.gseq_id) return false;
      // ignores strand and gseq_id, must check in advance
      for (int i=0;i<exons.Count();i++) {
         for (int j=0;j<m.exons.Count();j++) {
            if (exons[i]->start>m.exons[j]->end) continue;
            if (m.exons[j]->start>exons[i]->end) break;
            //-- overlap if we are here:
            return true;
            }
         }
      return false;
      }

    int exonOverlapIdx(uint s, uint e, int* ovlen=NULL, int start_idx=0) {
      //return the exons' index for the overlapping OR ADJACENT exon
      //ovlen, if given, will return the overlap length
      if (s>e) Gswap(s,e);
      s--;e++; //to also catch adjacent exons
      for (int i=start_idx;i<exons.Count();i++) {
            if (exons[i]->start>e) break;
            if (s>exons[i]->end) continue;
            //-- overlap if we are here:
            if (ovlen!=NULL) {
              s++;e--;
              int ovlend= (exons[i]->end>e) ? e : exons[i]->end;
              *ovlen= ovlend - ((s>exons[i]->start)? s : exons[i]->start)+1;
              }
            return i;
            } //for each exon
      *ovlen=0;
      return -1;
      }

    int exonOverlapLen(GffObj& m) {
      if (start>m.end || m.start>end) return 0;
      int i=0;
      int j=0;
      int ovlen=0;
      while (i<exons.Count() && j<m.exons.Count()) {
        uint istart=exons[i]->start;
        uint iend=exons[i]->end;
        uint jstart=m.exons[j]->start;
        uint jend=m.exons[j]->end;
        if (istart>jend) { j++; continue; }
        if (jstart>iend) { i++; continue; }
        //exon overlap
        uint ovstart=GMAX(istart,jstart);
        if (iend<jend) {
           ovlen+=iend-ovstart+1;
           i++;
           }
        else {
           ovlen+=jend-ovstart+1;
           j++;
           }
        }//while comparing exons
      return ovlen;
      }

    bool exonOverlap(GffObj* m) {
      return exonOverlap(*m);
      }
   //---------- coordinate transformation
   void xcoord(uint grstart, uint grend, char xstrand='+') {
     //relative coordinate transform, and reverse-complement transform if xstrand is '-'
     //does nothing if xstatus is the same already
     if (xstatus) {
          if (xstatus==xstrand && grstart==xstart && grend==xend) return;
          unxcoord();//restore original coordinates
          }
     xstatus=xstrand;
     xstart=grstart;
     xend=grend;
     if (CDstart>0) xcoordseg(CDstart, CDend);
     for (int i=0;i<exons.Count();i++) {
         xcoordseg(exons[i]->start, exons[i]->end);
         }
     if (xstatus=='-') {
       exons.Reverse();
       int flen=end-start;
       start=xend-end+1;
       end=start+flen;
       }
      else {
       start=start-xstart+1;
       end=end-xstart+1;
       }
     }

   //transform an arbitrary segment based on current xstatus/xstart-xend
   void xcoordseg(uint& segstart, uint &segend) {
     if (xstatus==0) return;
     if (xstatus=='-') {
       int flen=segend-segstart;
       segstart=xend-segend+1;
       segend=segstart+flen;
       return;
       }
     else {
       segstart=segstart-xstart+1;
       segend=segend-xstart+1;
       }
     }

   void unxcoord() { //revert back to absolute genomic/gff coordinates if xstatus==true
     if (xstatus==0) return; //nothing to do, no transformation appplied
     if (CDstart>0) unxcoordseg(CDstart, CDend);
     //restore all GffExon intervals too
     for (int i=0;i<exons.Count();i++) {
         unxcoordseg(exons[i]->start, exons[i]->end);
         }
     if (xstatus=='-') {
        exons.Reverse();
        int flen=end-start;
        start=xend-end+1;
        end=start+flen;
        }
      else {
        start=start+xstart-1;
        end=end+xstart-1;
        }
     xstatus=0;
     }
   void unxcoordseg(uint& astart, uint &aend) {
     //restore an arbitrary interval -- does NOT change the transform state!
     if (xstatus==0) return;
     if (xstatus=='-') {
        int flen=aend-astart;
        astart=xend-aend+1;
        aend=astart+flen;
        }
      else {
        astart=astart+xstart-1;
        aend=aend+xstart-1;
        }
     }
   //---------------------
   bool operator==(GffObj& d){
       return (gseq_id==d.gseq_id && start==d.start && end==d.end && strcmp(gffID, d.gffID)==0);
       }
   bool operator>(GffObj& d){
      if (gseq_id!=d.gseq_id) return (gseq_id>d.gseq_id);
      if (start==d.start) {
         if (getLevel()==d.getLevel()) {
             if (end==d.end) return (strcmp(gffID, d.gffID)>0);
                        else return (end>d.end);
             } else return (getLevel()>d.getLevel());
         } else return (start>d.start);
      }
   bool operator<(GffObj& d){
     if (gseq_id!=d.gseq_id) return (gseq_id<d.gseq_id);
     if (start==d.start) {
         if (getLevel()==d.getLevel()) {
            if (end==d.end) return strcmp(gffID, d.gffID)<0;
                     else return end<d.end;
            } else return (getLevel()<d.getLevel());
        } else return (start<d.start);
     }
   char* getID() { return gffID; }
   char* getGeneID() { return geneID; }
   char* getGeneName() { return gene_name; }
   void setGeneName(const char* gname) {
        GFREE(gene_name);
        if (gname) gene_name=Gstrdup(gname);
        }
   void setGeneID(const char* gene_id) {
        GFREE(geneID);
        if (gene_id) geneID=Gstrdup(gene_id);
        }
   int addSeg(GffLine* gfline);
   int addSeg(int fnid, GffLine* gfline);
   void getCDSegs(GArray<GffCDSeg>& cds);

   void updateExonPhase(); //for CDS-only features, updates GExon::phase
   void printGTab(FILE* fout, char** extraAttrs=NULL);
   void printGxfLine(FILE* fout, const char* tlabel, const char* gseqname,
          bool iscds, uint segstart, uint segend, int exidx,
		  char phase, bool gff3, bool cvtChars, char* dbuf, int dbuf_len);
   void printGxf(FILE* fout, GffPrintMode gffp=pgffExon,
             const char* tlabel=NULL, const char* gfparent=NULL, bool cvtChars=false);
   void printGtf(FILE* fout, const char* tlabel=NULL, bool cvtChars=false) {
      printGxf(fout, pgtfAny, tlabel, NULL, cvtChars);
      }
   void printGff(FILE* fout, const char* tlabel=NULL,
                                const char* gfparent=NULL, bool cvtChars=false) {
      printGxf(fout, pgffAny, tlabel, gfparent, cvtChars);
      }
   void printTranscriptGff(FILE* fout, char* tlabel=NULL,
                            bool showCDS=false, const char* gfparent=NULL, bool cvtChars=false) {
      if (isValidTranscript())
         printGxf(fout, showCDS ? pgffBoth : pgffExon, tlabel, gfparent, cvtChars);
      }
   void printExonList(FILE* fout); //print comma delimited list of exon intervals
   void printBED(FILE* fout, bool cvtChars, char* dbuf, int dbuf_len);
       //print a BED-12 line + GFF3 attributes in 13th field
   void printSummary(FILE* fout=NULL);
   void getCDS_ends(uint& cds_start, uint& cds_end);
   void mRNA_CDS_coords(uint& cds_start, uint& cds_end);
   char* getSpliced(GFaSeqGet* faseq, bool CDSonly=false, int* rlen=NULL,
           uint* cds_start=NULL, uint* cds_end=NULL, GList<GSeg>* seglst=NULL);
    char* getUnspliced(GFaSeqGet* faseq, int* rlen, GList<GSeg>* seglst);
   char* getSplicedTr(GFaSeqGet* faseq, bool CDSonly=true, int* rlen=NULL);
   //bool validCDS(GFaSeqGet* faseq); //has In-Frame Stop Codon ?
   bool empty() { return (start==0); }
};

typedef bool GffRecFunc(GffObj* gobj, void* usrptr1, void* usrptr2);
//user callback after parsing a mapping object:
// Returns: "done with it" status:
//   TRUE if gobj is no longer needed so it's FREEd upon return
//   FALSE if the user needs the gobj pointer and is responsible for
//             collecting and freeing all GffObj objects


//GSeqStat: collect basic stats about a common underlying genomic sequence
//          for multiple GffObj
class GSeqStat {
 public:
   int gseqid; //gseq id in the global static pool of gseqs
   char* gseqname; //just a pointer to the name of gseq
   int fcount;//number of features on this gseq
   uint mincoord;
   uint maxcoord;
   uint maxfeat_len; //maximum feature length on this genomic sequence
   GffObj* maxfeat;
   GSeqStat(int id=-1, char* name=NULL) {
     gseqid=id;
     gseqname=name;
     fcount=0;
     mincoord=MAXUINT;
     maxcoord=0;
     maxfeat_len=0;
     maxfeat=NULL;
     }
   bool operator>(GSeqStat& g) {
    return (gseqid>g.gseqid);
    }
   bool operator<(GSeqStat& g) {
    return (gseqid<g.gseqid);
    }
   bool operator==(GSeqStat& g) {
    return (gseqid==g.gseqid);
    }
};


int gfo_cmpByLoc(const pointer p1, const pointer p2);

class GfList: public GList<GffObj> {
  //just adding the option to sort by genomic sequence and coordinate
   bool mustSort;
 public:
   GfList(bool sortbyloc=false):GList<GffObj>(false,false,false) {
     //GffObjs in this list are NOT deleted when the list is cleared
     //-- for deallocation of these objects, call freeAll() or freeUnused() as needed
     mustSort=sortbyloc;
     }
   void sortedByLoc(bool v=true) {
     bool prev=mustSort;
     mustSort=v;
     if (fCount>0 && mustSort && !prev) {
       this->setSorted((GCompareProc*)gfo_cmpByLoc);
       }
     }
   void finalize(GffReader* gfr, bool mergeCloseExons,
                bool keepAttrs=false, bool noExonAttr=true);

   void freeAll() {
     for (int i=0;i<fCount;i++) {
       delete fList[i];
       fList[i]=NULL;
       }
     Clear();
     }
   void freeUnused() {
     for (int i=0;i<fCount;i++) {
       if (fList[i]->isUsed()) continue;
       //inform the children
       for (int c=0;c<fList[i]->children.Count();c++) {
          fList[i]->children[c]->parent=NULL;
          }
       delete fList[i];
       fList[i]=NULL;
       }
     Clear();
     }

};
/*
struct GfoHolder {
   //int idx; //position in GffReader::gflst array
   GffObj* gffobj;
   GfoHolder(GffObj* gfo=NULL) { //, int i=0) {
     //idx=i;
     gffobj=gfo;
     }
};
*/
class CNonExon { //utility class used in subfeature promotion
 public:
   //int idx;
   GffObj* parent;
   GffExon* exon;
   GffLine* gffline;
   //CNonExon(int i, GffObj* p, GffExon* e, GffLine* gl) {
   CNonExon(GffObj* p, GffExon* e, GffLine& gl) {
     parent=p;
     exon=e;
     //idx=i;
     gffline=new GffLine(gl);
     }
  ~CNonExon() {
     delete gffline;
     }
 };


class GffReader {
  friend class GffObj;
  friend class GffLine;
  char* linebuf;
  off_t fpos;
  int buflen;
 protected:
  union {
    uint8_t gff_type;
    struct {
       bool is_gff3: 1;  //GFF3 syntax was detected
       bool is_gtf:1; //GTF syntax was detected
       bool gtf_transcript:1; //has "transcript" features (2-level GTF)
       bool gtf_gene:1; //has "gene" features (3-level GTF ..Ensembl?)
       bool is_BED:1; //input is BED-12 format, possibly with attributes in 13th field
       bool is_TLF:1; //input is GFF3-like Transcript Line Format with exons= attribute
    };
  };
  bool gff_warns; //warn about duplicate IDs, etc. even when they are on different chromosomes
  char* lastReadNext;
  FILE* fh;
  char* fname;  //optional fasta file with the underlying genomic sequence to be attached to this reader
  GffLine* gffline;
  BEDLine* bedline;
  bool transcriptsOnly; //keep only transcripts w/ their exon/CDS features
  bool gene2exon;  // for childless genes: add an exon as the entire gene span
  GHash<int> discarded_ids; //for transcriptsOnly mode, keep track
                            // of discarded parent IDs
  GHash< GPVec<GffObj> > phash; //transcript_id => GPVec<GffObj>(false)
  //GHash<int> tids; //just for transcript_id uniqueness
  char* gfoBuildId(const char* id, const char* ctg);
  //void gfoRemove(const char* id, const char* ctg);
  GffObj* gfoAdd(GffObj* gfo);
  GffObj* gfoAdd(GPVec<GffObj>& glst, GffObj* gfo);
  GffObj* gfoReplace(GPVec<GffObj>& glst, GffObj* gfo, GffObj* toreplace);
  // const char* id, const char* ctg, char strand, GVec<GfoHolder>** glst, uint start, uint end
  bool pFind(const char* id, GPVec<GffObj>*& glst);
  GffObj* gfoFind(const char* id, GPVec<GffObj>* & glst, const char* ctg=NULL,
	                                         char strand=0, uint start=0, uint end=0);
  CNonExon* subfPoolCheck(GffLine* gffline, GHash<CNonExon>& pex, char*& subp_name);
  void subfPoolAdd(GHash<CNonExon>& pex, GffObj* newgfo);
  GffObj* promoteFeature(CNonExon* subp, char*& subp_name, GHash<CNonExon>& pex,
                                  bool keepAttr, bool noExonAttr);
#ifdef CUFFLINKS
     boost::crc_32_type  _crc_result;
#endif
 public:
  GPVec<GSeqStat> gseqtable; //table with all genomic sequences, but only current GXF gseq ID indices will have non-NULL
  //GffNames* names; //just a pointer to the global static Gff names repository
  GfList gflst; //accumulate GffObjs being read
  GffObj* newGffRec(GffLine* gffline, bool keepAttr, bool noExonAttr,
                               GffObj* parent=NULL, GffExon* pexon=NULL, GPVec<GffObj>* glst=NULL, bool replace_parent=false);
  GffObj* newGffRec(BEDLine* bedline, bool keepAttr, GPVec<GffObj>* glst=NULL);
  //GffObj* replaceGffRec(GffLine* gffline, bool keepAttr, bool noExonAttr, int replaceidx);
  GffObj* updateGffRec(GffObj* prevgfo, GffLine* gffline,
                                         bool keepAttr);
  GffObj* updateParent(GffObj* newgfh, GffObj* parent);
  bool addExonFeature(GffObj* prevgfo, GffLine* gffline, GHash<CNonExon>* pex=NULL, bool noExonAttr=false);
  GPVec<GSeqStat> gseqStats; //populated after finalize() with only the ref seqs in this file
  GffReader(FILE* f=NULL, bool t_only=false, bool sortbyloc=false):linebuf(NULL), fpos(0),
		  buflen(0), gff_type(0), gff_warns(gff_show_warnings), fh(f), fname(NULL), gffline(NULL),
		  bedline(NULL), transcriptsOnly(t_only), gene2exon(false), discarded_ids(true), phash(true), gseqtable(1,true),
		  gflst(sortbyloc), gseqStats(1, false) {
      GMALLOC(linebuf, GFF_LINELEN);
      buflen=GFF_LINELEN-1;
      gffnames_ref(GffObj::names);
      lastReadNext=NULL;
      }
  void init(FILE *f, bool t_only=false, bool sortbyloc=false, bool g2exon=false) {
      fname=NULL;
      fh=f;
      if (fh!=NULL) rewind(fh);
      fpos=0;
      gff_type=0;
      transcriptsOnly=t_only;
      gflst.sortedByLoc(sortbyloc);
      gene2exon=g2exon;
      }
  void set_gene2exon(bool v) { gene2exon=v;}
  void isBED(bool v=true) { is_BED=v; } //should be set before any parsing!
  void isTLF(bool v=true) { is_TLF=v; } //should be set before any parsing!
  GffReader(const char* fn, bool t_only=false, bool sort=false):linebuf(NULL), fpos(0),
	  		  buflen(0), gff_type(0), gff_warns(gff_show_warnings), fh(NULL), fname(NULL),
			  gffline(NULL), bedline(NULL), transcriptsOnly(t_only), gene2exon(false), discarded_ids(true),
			  phash(true), gseqtable(1,true), gflst(sort), gseqStats(1,false) {
      gff_warns=gff_show_warnings;
      gffnames_ref(GffObj::names);
      fname=Gstrdup(fn);
      fh=fopen(fname, "rb");
      GMALLOC(linebuf, GFF_LINELEN);
      buflen=GFF_LINELEN-1;
      lastReadNext=NULL;
      }

 ~GffReader() {
      delete gffline;
      gffline=NULL;
      fpos=0;
      if (fh && fh!=stdin) fclose(fh);
      gflst.freeUnused();
      gflst.Clear();
      discarded_ids.Clear();
      phash.Clear();
      GFREE(fname);
      GFREE(linebuf);
      GFREE(lastReadNext);
      gffnames_unref(GffObj::names);
      }

  void showWarnings(bool v=true) {
      gff_warns=v;
      gff_show_warnings=v;
      }

  GffLine* nextGffLine();
  BEDLine* nextBEDLine();

  // load all subfeatures, re-group them:
  void readAll(bool keepAttr=false, bool mergeCloseExons=false, bool noExonAttr=true);

  //only for well-formed files: BED or GxF where exons are strictly grouped by their transcript_id/Parent
  GffObj* readNext(); //user must free the returned GffObj* !

#ifdef CUFFLINKS
    boost::crc_32_type current_crc_result() const { return _crc_result; }
#endif

}; // end of GffReader

#endif
