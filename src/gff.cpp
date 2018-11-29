#include "gff.h"

GffNames* GffObj::names=NULL;
//global set of feature names, attribute names etc.
// -- common for all GffObjs in current application!

const uint GFF_MAX_LOCUS = 7000000; //longest known gene in human is ~2.2M, UCSC claims a gene for mouse of ~ 3.1 M
const uint GFF_MAX_EXON  =   30000; //longest known exon in human is ~11K
const uint GFF_MAX_INTRON= 6000000; //Ensembl shows a >5MB mouse intron
bool gff_show_warnings = false; //global setting, set by GffReader->showWarnings()
int gff_fid_mRNA=0; //mRNA (has CDS)
int gff_fid_transcript=1; // generic "transcript" feature
int gff_fid_exon=2; // "exon" feature

const uint gfo_flag_HAS_ERRORS       = 0x00000001;
const uint gfo_flag_CHILDREN_PROMOTED= 0x00000002;
const uint gfo_flag_IS_GENE          = 0x00000004;
const uint gfo_flag_IS_TRANSCRIPT    = 0x00000008;
const uint gfo_flag_HAS_GFF_ID       = 0x00000010; //found transcript feature line (GFF3 or GTF)
const uint gfo_flag_BY_EXON          = 0x00000020; //created by subfeature (exon) directly
const uint gfo_flag_DISCARDED        = 0x00000100;
const uint gfo_flag_LST_KEEP         = 0x00000200;
const uint gfo_flag_LEVEL_MSK        = 0x00FF0000;
const byte gfo_flagShift_LEVEL           = 16;

void gffnames_ref(GffNames* &n) {
  if (n==NULL) n=new GffNames();
  n->numrefs++;
}

void gffnames_unref(GffNames* &n) {
  if (n==NULL) GError("Error: attempt to remove reference to null GffNames object!\n");
  n->numrefs--;
  if (n->numrefs==0) { delete n; n=NULL; }
}

int classcode_rank(char c) {
	switch (c) {
		case '=': return 0; //intron chain match
		case 'c': return 2; //containment, perfect partial match (transfrag < reference)
		case 'k': return 6; // reverse containment (reference < transfrag)
		case 'm': return 6; // full span overlap with all reference introns either matching or retained
		case 'n': return 6; // partial overlap transfrag with at least one intron retention
		case 'j': return 6; // multi-exon transfrag with at least one junction match
		case 'e': return 12; // single exon transfrag partially overlapping an intron of reference (possible pre-mRNA fragment)
		case 'o': return 14; // other generic exon overlap
		case 's': return 16; //"shadow" - an intron overlaps with a ref intron on the opposite strand (wrong strand mapping?)
		case 'x': return 18; // generic overlap on opposite strand (usually wrong strand mapping)
		case 'i': return 20; // intra-intron (transfrag fully contained within a reference intron)
		case 'y': return 30; // no exon overlap: ref exons fall within transfrag introns!
		case 'p': return 90; //polymerase run
		case 'r': return 92; //repeats
		case 'u': return 94; //intergenic
		case  0 : return 100;
		 default: return 96;
		}
}

const char* strExonType(char xtype) {
	static const char* extbl[7]={"None", "start_codon", "stop_codon", "CDS", "UTR", "CDS_UTR", "exon"};
	if (xtype>0 && xtype<7)
	   return extbl[(int)xtype];
	else return "NULL";
}

int gfo_cmpByLoc(const pointer p1, const pointer p2) {
 GffObj& g1=*((GffObj*)p1);
 GffObj& g2=*((GffObj*)p2);
 if (g1.gseq_id==g2.gseq_id) {
             if (g1.start!=g2.start)
                    return (int)(g1.start-g2.start);
               else if (g1.getLevel()!=g2.getLevel())
                        return (int)(g1.getLevel()-g2.getLevel());
                    else
                        if (g1.end!=g2.end)
                              return (int)(g1.end-g2.end);
                        else return strcmp(g1.getID(), g2.getID());
             }
             else //return (int)(g1.gseq_id-g2.gseq_id); // input order !
            	 return strcmp(g1.getGSeqName(), g2.getGSeqName()); //lexicographic !
}

char* GffLine::extractGFFAttr(char* & infostr, const char* oline, const char* attr, bool caseStrict, bool enforce_GTF2, int* rlen) {
 //parse a key attribute and remove it from the info string
 //(only works for attributes that have values following them after ' ' or '=')
 static const char GTF2_ERR[]="Error parsing attribute %s ('\"' required for GTF) at line:\n%s\n";
 int attrlen=strlen(attr);
 char cend=attr[attrlen-1];
 //char* pos = (caseStrict) ? strstr(info, attr) : strifind(info, attr);
 //must make sure attr is not found in quoted text
 char* pos=infostr;
 char prevch=0;
 bool in_str=false;
 bool notfound=true;
 int (*strcmpfn)(const char*, const char*, int) = caseStrict ? Gstrcmp : Gstricmp;
 while (notfound && *pos) {
   char ch=*pos;
   if (ch=='"') {
     in_str=!in_str;
     pos++;
     prevch=ch;
     continue;
     }
   if (!in_str && (prevch==0 || prevch==' ' || prevch == ';')
          && strcmpfn(attr, pos, attrlen)==0) {
      //attr match found
      //check for word boundary on right
      char* epos=pos+attrlen;
      if (cend=='=' || cend==' ' || *epos==0 || *epos==' ') {
        notfound=false;
        break;
        }
      //not a perfect match, move on
      pos=epos;
      prevch=*(pos-1);
      continue;
      }
   //not a match or in_str
   prevch=ch;
   pos++;
   }
 if (notfound) return NULL;
 char* vp=pos+attrlen;
 while (*vp==' ') vp++;
 if (*vp==';' || *vp==0) {
      GMessage("Warning: parsing value of GFF attribute \"%s\" at line:\n%s\n", attr, oline);
      return NULL;
 }
 bool dq_enclosed=false; //value string enclosed by double quotes
 if (*vp=='"') {
     dq_enclosed=true;
     vp++;
     }
 if (enforce_GTF2 && !dq_enclosed)
      GError(GTF2_ERR, attr, oline);
 char* vend=vp;
 if (dq_enclosed) {
    while (*vend!='"' && *vend!=';' && *vend!=0) vend++;
    }
 else {
    while (*vend!=';' && *vend!=0) vend++;
    }
 if (enforce_GTF2 && *vend!='"')
     GError(GTF2_ERR, attr, oline);
 char *r=Gstrdup(vp, vend-1);
 if (rlen) *rlen = vend-vp;
 //-- now remove this attribute from the info string
 while (*vend!=0 && (*vend=='"' || *vend==';' || *vend==' ')) vend++;
 if (*vend==0) vend--;
 for (char *src=vend, *dest=pos;;src++,dest++) {
   *dest=*src;
   if (*src==0) break;
 }
 return r;
}
BEDLine::BEDLine(GffReader* reader, const char* l): skip(true), dupline(NULL), line(NULL), llen(0),
		gseqname(NULL), fstart(0), fend(0), strand(0), ID(NULL), info(NULL),
		cds_start(0), cds_end(0), cds_phase(0), exons(1) {
  if (reader==NULL || l==NULL) return;
  llen=strlen(l);
  GMALLOC(line,llen+1);
  memcpy(line, l, llen+1);
  GMALLOC(dupline, llen+1);
  memcpy(dupline, l, llen+1);
  char* t[14];
  int i=0;
  int tidx=1;
  t[0]=line;
  if (startsWith(line, "browser ") || startsWith(line, "track "))
	  return;
  while (line[i]!=0) {
   if (line[i]=='\t') {
    line[i]=0;
    t[tidx]=line+i+1;
    tidx++;
    //if (tidx>13) { extra=t[13]; break; }
    //our custom BED-13 format, with GFF3 attributes in 13th column
    if (tidx>12) { info=t[12]; break; }
   }
   i++;
  }
  /* if (tidx<6) { // require BED-6+ lines
   GMessage("Warning: 6+ BED columns expected, instead found:\n%s\n", l);
   return;
   }
  */
  gseqname=t[0];
  char* p=t[1];
  if (!parseUInt(p,fstart)) {
    GMessage("Warning: invalid BED start coordinate at line:\n%s\n",l);
    return;
    }
  ++fstart; //BED start is 0 based
  p=t[2];
  if (!parseUInt(p,fend)) {
    GMessage("Warning: invalid BED end coordinate at line:\n%s\n",l);
    return;
    }
  if (fend<fstart) Gswap(fend,fstart); //make sure fstart<=fend, always
  if (tidx>5) {
	  strand=*t[5];
	  if (strand!='-' && strand !='.' && strand !='+') {
		  GMessage("Warning: unrecognized BED strand at line:\n%s\n",l);
		  return;
	  }
  }
  else strand='.';
  //if (tidx>12) ID=t[12];
  //        else ID=t[3];
  ID=t[3];
  //now parse the exons, if any
  if (tidx>11) {
	  int numexons=0;
	  p=t[9];
	  if (!parseInt(p, numexons) || numexons<=0) {
	      GMessage("Warning: invalid BED block count at line:\n%s\n",l);
	      return;
	  }
	  char** blen;
	  char** bstart;
	  GMALLOC(blen, numexons * sizeof(char*));
	  GMALLOC(bstart, numexons * sizeof(char*));
	  i=0;
	  int b=1;
	  blen[0]=t[10];
	  while (t[10][i]!=0 && b<=numexons) {
		if (t[10][i]==',') {
			t[10][i]=0;
			if (b<numexons)
			  blen[b]=t[10]+i+1;
			b++;
		}
		i++;
	  }
	  b=1;i=0;
	  bstart[0]=t[11];
	  while (t[11][i]!=0 && b<=numexons) {
		if (t[11][i]==',') {
			t[11][i]=0;
			if (b<numexons)
			  bstart[b]=t[11]+i+1;
			b++;
		}
		i++;
	  }
	  GSeg ex;
	  for (i=0;i<numexons;++i) {
		  int exonlen;
		  if (!strToInt(blen[i], exonlen) || exonlen<=0) {
		      GMessage("Warning: invalid BED block size %s at line:\n%s\n",blen[i], l);
		      return;
		  }
		  int exonstart;
		  if (!strToInt(bstart[i], exonstart) || exonstart<0) {
		      GMessage("Warning: invalid BED block start %s at line:\n%s\n",bstart[i], l);
		      return;
		  }
		  if (i==0 && exonstart!=0) {
			  GMessage("Warning: first BED block start is %d>0 at line:\n%s\n",exonstart, l);
			  return;
		  }
		  exonstart+=fstart;
		  uint exonend=exonstart+exonlen-1;
		  if ((uint)exonstart>fend || exonend>fend) {
			  GMessage("Warning: BED exon %d-%d is outside record boundary at line:\n%s\n",exonstart,exonend, l);
			  return;
		  }
		  ex.start=exonstart;ex.end=exonend;
		  exons.Add(ex);
	  }
	  GFREE(blen);
	  GFREE(bstart);
  }
  else { //take it as single-exon transcript
	  GSeg v(fstart, fend);
	  exons.Add(v);
  }
  if (info!=NULL) {
	  char* cdstr=GffLine::extractGFFAttr(info, dupline, "CDS=");
	  if (cdstr) {
		 char* p=strchr(cdstr, ':');
		 if (p!=NULL) {
			*p='\0'; ++p;
		 }
		if (strToUInt(cdstr, cds_start) && cds_start>=fstart-1) {
			++cds_start;
			if (!strToUInt(p, cds_end) || cds_end>fend) {
				GMessage("Warning: invalid CDS (%d-%d) discarded for line:\n%s\n",
						    cds_start, cds_end, dupline);
				cds_start=0;
				cds_end=0; //invalid CDS coordinates
			}
		}
		char* cdstr_phase=NULL;
		if (cds_start>0 && (cdstr_phase=GffLine::extractGFFAttr(info, dupline, "CDSphase="))!=NULL) {
			cds_phase=cdstr_phase[0];
			GFREE(cdstr_phase);
		}
		GFREE(cdstr);
	  }
  }
  skip=false;
}

GffLine::GffLine(GffReader* reader, const char* l): _parents(NULL), _parents_len(0),
		dupline(NULL), line(NULL), llen(0), gseqname(NULL), track(NULL),
		ftype(NULL), ftype_id(-1), info(NULL), fstart(0), fend(0), qstart(0), qend(0), qlen(0),
		score(0), strand(0), flags(0), exontype(0), phase(0), cds_start(0), cds_end(0), exons(),
		gene_name(NULL), gene_id(NULL),
		parents(NULL), num_parents(0), ID(NULL) {
 llen=strlen(l);
 GMALLOC(line,llen+1);
 memcpy(line, l, llen+1);
 GMALLOC(dupline, llen+1);
 memcpy(dupline, l, llen+1);
 skipLine=1; //reset only if it reaches the end of this function
 char* t[9];
 int i=0;
 int tidx=1;
 t[0]=line;
 char fnamelc[128];
 while (line[i]!=0) {
  if (line[i]=='\t') {
   line[i]=0;
   t[tidx]=line+i+1;
   tidx++;
   if (tidx>8) break;
   }
  i++;
  }
 if (tidx<8) { // ignore non-GFF lines
  // GMessage("Warning: error parsing GFF/GTF line:\n%s\n", l);
  return;
  }
 gseqname=t[0];
 track=t[1];
 ftype=t[2];
 info=t[8];
 char* p=t[3];
 if (!parseUInt(p,fstart)) {
   //chromosome_band entries in Flybase
   GMessage("Warning: invalid start coordinate at line:\n%s\n",l);
   return;
   }
 p=t[4];
 if (!parseUInt(p,fend)) {
   GMessage("Warning: invalid end coordinate at line:\n%s\n",l);
   return;
   }
 if (fend<fstart) Gswap(fend,fstart); //make sure fstart<=fend, always
 p=t[5];
 if (p[0]=='.' && p[1]==0) {
  score=0;
  }
 else {
  if (!parseDouble(p,score))
       GError("Error parsing feature score from GFF line:\n%s\n",l);
  }
 strand=*t[6];
 if (strand!='+' && strand!='-' && strand!='.')
     GError("Error parsing strand (%c) from GFF line:\n%s\n",strand,l);
 phase=*t[7]; // must be '.', '0', '1' or '2'
 // exon/CDS/mrna filter
 strncpy(fnamelc, ftype, 127);
 fnamelc[127]=0;
 strlower(fnamelc); //convert to lower case
 bool is_t_data=false;
 bool someRNA=false;
 if (strstr(fnamelc, "utr")!=NULL) {
	 exontype=exgffUTR;
	 is_exon=true;
	 is_t_data=true;
 }
 else if (endsWith(fnamelc, "exon")) {
	 exontype=exgffExon;
	 is_exon=true;
	 is_t_data=true;
 }
 else if (strstr(fnamelc, "stop") &&
		 (strstr(fnamelc, "codon") || strstr(fnamelc, "cds"))){
	 exontype=exgffStop;
	 is_exon=true;
	 is_cds=true; //though some place it outside the last CDS segment
	 is_t_data=true;
 }
 else if (strstr(fnamelc, "start") &&
		 ((strstr(fnamelc, "codon")!=NULL) || strstr(fnamelc, "cds")!=NULL)){
	 exontype=exgffStart;
	 is_exon=true;
	 is_cds=true;
	 is_t_data=true;
 }
 else if (strcmp(fnamelc, "cds")==0) {
	 exontype=exgffCDS;
	 is_exon=true;
	 is_cds=true;
	 is_t_data=true;
 }
 else if (startsWith(fnamelc, "intron") || endsWith(fnamelc, "intron")) {
	 exontype=exgffIntron;
 }
 else if ((someRNA=endsWith(fnamelc,"rna")) || endsWith(fnamelc,"transcript")) { // || startsWith(fnamelc+1, "rna")) {
	 is_transcript=true;
	 is_t_data=true;
	 if (someRNA) ftype_id=GffObj::names->feats.addName(ftype);
 }
 else if (endsWith(fnamelc, "gene") || startsWith(fnamelc, "gene")) {
	 is_gene=true;
	 is_t_data=true; //because its name will be attached to parented transcripts
 }
 char* Parent=NULL;
 /*
  Rejecting non-transcript lines early if only transcripts are requested ?!
  It would be faster to do this here but there are GFF cases when we reject a parent feature here
  (e.g. protein with 2 CDS children) and then their exon/CDS children show up and
  get assigned to an implicit parent mRNA
  The solution is to still load this parent as GffObj for now and BAN it later
  so its children get dismissed/discarded as well.
 */
 char *gtf_tid=NULL;
 char *gtf_gid=NULL;
 if (reader->is_gff3 || reader->gff_type==0) {
	ID=extractAttr("ID=",true);
	Parent=extractAttr("Parent=",true);
	if (reader->gff_type==0) {
		if (ID!=NULL || Parent!=NULL) reader->is_gff3=true;
			else { //check if it looks like a GTF
				gtf_tid=extractAttr("transcript_id", true, true);
				if (gtf_tid==NULL) {
					gtf_gid=extractAttr("gene_id", true, true);
					if (gtf_gid==NULL) return; //cannot determine file type yet
				}
				reader->is_gtf=true;
			}
	}
 }

 if (reader->is_gff3) {
	 //parse as GFF3
	 //if (ID==NULL && Parent==NULL) return; //silently ignore unidentified/unlinked features
	 if (ID!=NULL) {
		 //has ID attr so it's likely to be a parent feature
		 //look for explicit gene name
		 gene_name=extractAttr("gene_name=");
		 if (gene_name==NULL) {
			 gene_name=extractAttr("geneName=");
			 if (gene_name==NULL) {
				 gene_name=extractAttr("gene_sym=");
				 if (gene_name==NULL) {
					 gene_name=extractAttr("gene=");
				 }
			 }
		 }
		 gene_id=extractAttr("geneID=");
		 if (gene_id==NULL) {
			 gene_id=extractAttr("gene_id=");
		 }
		 if (is_gene) { //WARNING: this might be mislabeled (e.g. TAIR: "mRNA_TE_gene")
			 //special case: keep the Name and ID attributes of the gene feature
			 if (gene_name==NULL)
				 gene_name=extractAttr("Name=");
			 if (gene_id==NULL) //the ID is also gene_id in this case
				 gene_id=Gstrdup(ID);
			 //skip=false;
			 //return;
			 //-- we don't care about gene parents.. unless it's a mislabeled "gene" feature
			 //GFREE(Parent);
		 } //gene feature (probably)

		 //--parse exons for TLF
         char* exonstr=extractAttr("exons=");
         if (exonstr) {
        	  GDynArray<char*> exs;
        	  strsplit(exonstr, exs, ',');
        	  char* exoncountstr=extractAttr("exonCount=");
        	  if (exoncountstr) {
        		  int exoncount=0;
        		  if (!strToInt(exoncountstr, exoncount) || exoncount!=(int)exs.Count())
        			  GMessage("Warning: exonCount attribute value doesn't match the exons attribute!\n");
        		  GFREE(exoncountstr);
        	  }
        	  GSeg ex;
        	  bool exons_valid=true;
        	  for (uint i=0;i<exs.Count();++i) {
        		  char* p=strchr(exs[i], '-');
        		  if (p==NULL) {
        			  exons_valid=false;
        			  break;
        		  }
        		  *p='\0'; ++p;
        		  int xstart=0, xend=0;
        		  if (!strToInt(exs[i], xstart) || xstart<(int)fstart) {
        			  exons_valid=false;
        			  break;
        		  }
        		  if (!strToInt(p, xend) || xend<1 || xend>(int)fend) {
        			  exons_valid=false;
        			  break;
        		  }
        		  ex.start=(uint)xstart;ex.end=(uint)xend;
        		  exons.Add(ex);
        	  }
        	  if (!exons_valid)
        		  exons.Clear();
         	  GFREE(exonstr);
         	  if (exons_valid) {
         		  char* cdstr=extractAttr("CDS=");
         		  if (cdstr) {
         			 char* p=strchr(cdstr, ':');
         			 if (p!=NULL) {
         				*p='\0'; ++p;
         			 }
         			bool validCDS=(p!=NULL);
         			if (validCDS && strToUInt(cdstr, cds_start) && cds_start>=fstart) {
         				if (!strToUInt(p, cds_end) || cds_end>fend) {
         					validCDS=false;
         				}
         			}
         			if (!validCDS || (int)cds_start<=0 || (int)cds_end<=0) {
    				    GMessage("Warning: invalid CDS (%d-%d) discarded for line:\n%s\n",
    						    cds_start, cds_end, dupline);
    				    cds_start=0;
    				    cds_end=0;
         			}
         			char* cds_phase=NULL;
         			if (validCDS && (cds_phase=extractAttr("CDSphase="))!=NULL) {
         				phase=cds_phase[0];
         				GFREE(cds_phase);
         			}
         			GFREE(cdstr);
         		  }
         	  }//valid exons=
         } //has exons= attribute

	 }// has GFF3 ID
	 if (Parent!=NULL) {
		 //keep Parent attr
		 //parse multiple parents
		 num_parents=1;
		 p=Parent;
		 int last_delim_pos=-1;
		 while (*p!=';' && *p!=0) {
			 if (*p==',' && *(p+1)!=0 && *(p+1)!=';') {
				 num_parents++;
				 last_delim_pos=(p-Parent);
			 }
			 p++;
		 }
		 _parents_len=p-Parent+1;
		 _parents=Parent;
		 GMALLOC(parents, num_parents*sizeof(char*));
		 parents[0]=_parents;
		 int i=1;
		 if (last_delim_pos>0) {
			 for (p=_parents+1;p<=_parents+last_delim_pos;p++) {
				 if (*p==',') {
					 char* ep=p-1;
					 while (*ep==' ' && ep>_parents) ep--;
					 *(ep+1)=0; //end the string there
					 parents[i]=p+1;
					 i++;
				 }
			 }
		 }
	 } //has Parent field
	 //parse other potentially useful GFF3 attributes
	 if ((p=strstr(info,"Target="))!=NULL) { //has Target attr
		 p+=7;
		 while (*p!=';' && *p!=0 && *p!=' ') p++;
		 if (*p!=' ') {
			 GError("Error parsing target coordinates from GFF line:\n%s\n",l);
		 }
		 if (!parseUInt(p,qstart))
			 GError("Error parsing target start coordinate from GFF line:\n%s\n",l);
		 if (*p!=' ') {
			 GError("Error parsing next target coordinate from GFF line:\n%s\n",l);
		 }
		 p++;
		 if (!parseUInt(p,qend))
			 GError("Error parsing target end coordinate from GFF line:\n%s\n",l);
	 }
	 if ((p=strifind(info,"Qreg="))!=NULL) { //has Qreg attr
		 p+=5;
		 if (!parseUInt(p,qstart))
			 GError("Error parsing target start coordinate from GFF line:\n%s\n",l);
		 if (*p!='-') {
			 GError("Error parsing next target coordinate from GFF line:\n%s\n",l);
		 }
		 p++;
		 if (!parseUInt(p,qend))
			 GError("Error parsing target end coordinate from GFF line:\n%s\n",l);
		 if (*p=='|' || *p==':') {
			 p++;
			 if (!parseUInt(p,qlen))
				 GError("Error parsing target length from GFF Qreg|: \n%s\n",l);
		 }
	 }//has Qreg attr
	 if (qlen==0 && (p=strifind(info,"Qlen="))!=NULL) {
		 p+=5;
		 if (!parseUInt(p,qlen))
			 GError("Error parsing target length from GFF Qlen:\n%s\n",l);
	 }
 } //GFF3
 else { // GTF syntax
	 if (reader->transcriptsOnly && !is_t_data) {
		 return; //alwasys skip unrecognized non-transcript features in GTF
	 }
	 if (is_gene) {
		 reader->gtf_gene=true;
		 ID = (gtf_tid!=NULL) ? gtf_tid : extractAttr("transcript_id", true, true); //Ensemble GTF might lack this
		 gene_id = (gtf_gid!=NULL) ? gtf_gid : extractAttr("gene_id", true, true);
		 if (ID==NULL) {
			 // no transcript_id -- this is not a valid GTF2 format, but Ensembl
			 //is being known to add "gene" features with only gene_id in their GTF
			 if (gene_id!=NULL) { //likely a gene feature line (Ensembl!)
			 		 ID=Gstrdup(gene_id); //take over as ID
			 }
		 }
		 // else if (strcmp(gene_id, ID)==0) //GENCODE v20 gene feature ?
	 }
	 else if (is_transcript) {
		 ID = (gtf_tid!=NULL) ? gtf_tid : extractAttr("transcript_id", true, true);
		//gene_id=extractAttr("gene_id"); // for GTF this is the only attribute accepted as geneID
		 if (ID==NULL) {
			 	 //something is wrong here, cannot parse the GTF ID
				 GMessage("Warning: invalid GTF record, transcript_id not found:\n%s\n", l);
				 return;
		 }
		 gene_id = (gtf_gid!=NULL) ? gtf_gid : extractAttr("gene_id", true, true);
		if (gene_id!=NULL)
			Parent=Gstrdup(gene_id);
		reader->gtf_transcript=true;
		is_gtf_transcript=1;
	 } else { //must be an exon type
		 Parent = (gtf_tid!=NULL) ? gtf_tid : extractAttr("transcript_id", true, true);
		 gene_id = (gtf_gid!=NULL) ? gtf_gid : extractAttr("gene_id", true, true); // for GTF this is the only attribute accepted as geneID
		 //old pre-GTF2 formats like Jigsaw's (legacy)
		 if (Parent==NULL && exontype==exgffExon) {
			 if (startsWith(track,"jigsaw")) {
				 is_cds=true;
				 strcpy(track,"jigsaw");
				 p=strchr(info,';');
				 if (p==NULL) { Parent=Gstrdup(info); info=NULL; }
				 else { Parent=Gstrdup(info,p-1);
				 info=p+1;
				 }
			 }
		 }
		 if (Parent==NULL) {
			 	 //something is wrong here couldn't parse the transcript ID for this feature
				 GMessage("Warning: invalid GTF record, transcript_id not found:\n%s\n", l);
				 return;
		 }
	 }
	 //more GTF attribute parsing
	 gene_name=extractAttr("gene_name");
	 if (gene_name==NULL) {
		 gene_name=extractAttr("gene_sym");
		 if (gene_name==NULL) {
			 gene_name=extractAttr("gene");
			 if (gene_name==NULL)
				 gene_name=extractAttr("genesymbol");
		 }
	 }
	 //prepare GTF for parseAttr by adding '=' character after the attribute name
	 //for all attributes
	 p=info;
	 bool noed=true; //not edited after the last delim
	 bool nsp=false; //non-space found after last delim
	 while (*p!=0) {
		 if (*p==' ') {
			 if (nsp && noed) {
				 *p='=';
				 noed=false;
				 p++;
				 continue;
			 }
		 }
		 else nsp=true; //non-space
		 if (*p==';') { noed=true; nsp=false; }
		 p++;
	 }
	 //-- GTF prepare parents[] if Parent found
	 if (Parent!=NULL) { //GTF transcript_id found as a parent
		 _parents=Parent;
		 num_parents=1;
		 _parents_len=strlen(Parent)+1;
		 GMALLOC(parents, sizeof(char*));
		 parents[0]=_parents;
	 }
 } //GTF


 if (ID==NULL && parents==NULL) {
	 if (reader->gff_warns)
		 GMessage("Warning: could not parse ID or Parent from GFF line:\n%s\n",dupline);
	 return; //skip
 }
 skipLine=0;
}


void GffObj::addCDS(uint cd_start, uint cd_end, char phase) {
  if (cd_start>=this->start) {
        this->CDstart=cd_start;
        if (strand=='+') this->CDphase=phase;
        }
      else this->CDstart=this->start;
  if (cd_end<=this->end) {
      this->CDend=cd_end;
      if (strand=='-') this->CDphase=phase;
      }
     else this->CDend=this->end;
  isTranscript(true);
  exon_ftype_id=gff_fid_exon;
  if (monoFeature()) {
     if (exons.Count()==0) addExon(this->start, this->end,0,'.',0,0,false,exgffExon);
            else exons[0]->exontype=exgffExon;
     }
}

int GffObj::addExon(GffReader* reader, GffLine* gl, bool keepAttr, bool noExonAttr) {
  //this will make sure we have the right subftype_id!
  //int subf_id=-1;
  if (!isTranscript() && gl->exontype>0) {
          isTranscript(true);
          exon_ftype_id=gff_fid_exon;
          if (exons.Count()==1) exons[0]->exontype=exgffExon;
          }
  if (isTranscript()) {
     if (exon_ftype_id<0) {//exon_ftype_id=gff_fid_exon;
          if (gl->exontype>0) exon_ftype_id=gff_fid_exon;
                         else exon_ftype_id=names->feats.addName(gl->ftype);
          }
     //any recognized mRNA segment gets the generic "exon" type (also applies to CDS)
     if (gl->exontype==0 && !gl->is_transcript) {
          //extraneous mRNA feature, discard
          if (reader->gff_warns)
            GMessage("Warning: discarding unrecognized transcript subfeature '%s' of %s\n",
                gl->ftype, gffID);
          return -1;
          }
     }
  else { //non-mRNA parent feature, check this subf type
    int subf_id=names->feats.addName(gl->ftype);
    if (exon_ftype_id<0 || exons.Count()==0) //never assigned a subfeature type before (e.g. first exon being added)
       exon_ftype_id=subf_id;
     else {
       if (exon_ftype_id!=subf_id) {
         //
         if (exon_ftype_id==ftype_id && exons.Count()==1 && exons[0]->start==start && exons[0]->end==end) {
            //the existing exon was just a dummy one created by default, discard it
            exons.Clear();
            covlen=0;
            exon_ftype_id=subf_id; //allow the new subfeature to completely takeover
            }
         else { //multiple subfeatures, prefer those with
             if (reader->gff_warns)
               GMessage("GFF Warning: multiple subfeatures (%s and %s) found for %s, discarding ",
                  names->feats.getName(subf_id), names->feats.getName(exon_ftype_id),gffID);
            if (gl->exontype!=0) { //new feature is an exon, discard previously parsed subfeatures
               if (reader->gff_warns) GMessage("%s.\n", names->feats.getName(exon_ftype_id));
               exon_ftype_id=subf_id;
               exons.Clear();
               covlen=0;
               }
              else { //discard new feature
               if (reader->gff_warns) GMessage("%s.\n", names->feats.getName(subf_id));
               return -1; //skip this 2nd subfeature type for this parent!
               }
            }
         } //incoming subfeature is of different type
       } //new subfeature type
    } //non-mRNA parent
  int eidx=addExon(gl->fstart, gl->fend, gl->score, gl->phase,
         gl->qstart,gl->qend, gl->is_cds, gl->exontype);
  if (eidx<0) return eidx; //this should never happen
  if (keepAttr) {
     if (noExonAttr) {
         if (attrs==NULL) //place the parsed attributes directly at transcript level
           parseAttrs(attrs, gl->info);
         }
       else { //need all exon-level attributes
         parseAttrs(exons[eidx]->attrs, gl->info, true);
         }
      }
  return eidx;
}


int GffObj::addExon(uint segstart, uint segend, double sc, char fr, int qs, int qe, bool iscds, char exontype) {
  if (segstart>segend) { Gswap(segstart, segend); }
  if (exons.Count()==0) {
      if (iscds) isCDS=true; //for now, assume CDS only if first "exon" given is a CDS
      if (exon_ftype_id<0) {
         exon_ftype_id = (isTranscript() || isGene()) ? gff_fid_exon : ftype_id;
      }
  }
  //special treatment of start/stop codon features, they might be broken/split between exons (?)
  //we consider these as part of the CDS
  if (exontype==exgffStart || exontype==exgffStop) {
	 iscds=true;
	 //if (CDend==0 || segend>CDend) CDend=segend;
	 //if (CDstart==0 || segstart<CDstart) CDstart=segstart;
   }
   if (iscds) { //update CDS anchors:
     if (CDstart==0 || segstart<CDstart)  {
           CDstart=segstart;
           if (exontype==exgffCDS && strand=='+') CDphase=fr;
           }
     if (CDend==0 || segend>CDend) {
           CDend=segend;
           if (exontype==exgffCDS && strand=='-') CDphase=fr;
           }
   }
   else { // not a CDS/start/stop
    isCDS=false; //not a CDS-only feature
   }
  //if (exontype==exgffStart || exontype==exgffStop) exontype=exgffCDS;
  if (qs || qe) {
    if (qs>qe) Gswap(qs,qe);
    if (qs==0) qs=1;
	}
  int ovlen=0;
  if (exontype>0) { //check for overlaps between exon-type segments
	  int oi=-1;
      while ((oi=exonOverlapIdx(segstart, segend, &ovlen, oi+1))>=0) {
    	//ovlen==0 for adjacent segment
	    if (exons[oi]->exontype>0) {
	    	if (exons[oi]->start<=segstart && exons[oi]->end>=segend) {
	    		//existing feature includes this segment
	    		return oi;
	    	}
	    	else {
			 expandExon(oi, segstart, segend, exgffExon, sc, fr, qs, qe);
			 return oi;
	    	}
	    }
	  }
      /*
      if (oi>=0) { //overlap existing segment (exon)

         if (ovlen==0) {
			 //adjacent exon-type segments will be merged
			 //e.g. CDS to (UTR|exon|stop_codon)

			  if ((exons[oi]->exontype>=exgffUTR && exontype==exgffCDS) ||
				  (exons[oi]->exontype==exgffCDS && exontype>=exgffUTR)) {
					expandExon(oi, segstart, segend, exgffCDSUTR, sc, fr, qs, qe);
					return oi;
					}
			  //CDS adjacent to stop_codon: UCSC does (did?) this
			  if ((exons[oi]->exontype==exgffStop && exontype==exgffCDS) ||
				  (exons[oi]->exontype==exgffCDS && exontype==exgffStop)) {
					expandExon(oi, segstart, segend, exgffCDS, sc, fr, qs, qe);
					return oi;
					}

        }
		//segment inclusion: only allow this for CDS within exon, stop_codon within (CDS|UTR|exon),
        //                   start_codon within (CDS|exon)
        if (exons[oi]->start<=segstart && exons[oi]->end>=segend) {
          //larger segment given first, now the smaller included one is redundant
           if (exons[oi]->exontype>exontype &&
             !(exons[oi]->exontype==exgffUTR && exontype==exgffCDS)) {
              return oi; //only used to store attributes from current GffLine
              }
           else {
          	 //if (gff_show_warnings && (exons[oi]->start<segstart || exons[oi]->end>segend)) {
          	//	 GMessage("GFF Warning: unusual segment inclusion: %s(%d-%d) within %s(%d-%d) (ID=%s)\n",
          	//			 strExonType(exontype), segstart, segend, strExonType(exons[oi]->exontype),
          	//			 exons[oi]->start, exons[oi]->end, this->gffID);
          	 //}
            return oi;
           }
        }
        if (exontype>exons[oi]->exontype &&
             segstart<=exons[oi]->start && segend>=exons[oi]->end &&
             !(exontype==exgffUTR && exons[oi]->exontype==exgffCDS)) {
               //smaller segment given first, so we have to enlarge it
			  expandExon(oi, segstart, segend, exontype, sc, fr, qs, qe);
				//this should also check for overlapping next exon (oi+1) ?
              return oi;
              }
        //there is also the special case of "ribosomal slippage exception" (programmed frameshift)
        //where two CDS segments may actually overlap for 1 or 2 bases, but there should be only one encompassing exon
		//if (ovlen>2 || exons[oi]->exontype!=exgffCDS || exontype!=exgffCDS) {
		// had to relax this because of some weird UCSC annotations with exons partially overlapping the CDS segments

		 if ((ovlen>2 || ovlen==0) || exons[oi]->exontype!=exgffCDS || exontype!=exgffCDS) {
		  //if (gff_show_warnings)
		   //	 GMessage("GFF Warning: merging overlapping/adjacent feature segment %s (%d-%d) with %s (%d-%d) for GFF ID %s on %s\n",
		   //		 strExonType(exontype), segstart, segend, strExonType(exons[oi]->exontype), exons[oi]->start, exons[oi]->end, gffID, getGSeqName());
			expandExon(oi, segstart, segend, exontype, sc, fr, qs, qe);
			return oi;
		 }
		// else add the segment if the overlap is small and between two CDS segments
		//TODO: we might want to add an attribute here with the slippage coordinate and size?
        covlen-=ovlen;
		}//overlap or adjacent to existing segment
		*/
   } //exon type, check for overlap with existing exons
   // create & add the new segment
   /*
   if (start>0 && exontype==exgffCDS && exons.Count()==0) {
      //adding a CDS directly as the first subfeature of a declared parent
      segstart=start;
      segend=end;
      }
   */
   GffExon* enew=new GffExon(segstart, segend, sc, fr, qs, qe, exontype);
   int eidx=exons.Add(enew);
   if (eidx<0) {
    //this would actually be acceptable if the object is a "Gene" and "exons" are in fact isoforms
     if (gff_show_warnings)
       GMessage("GFF Warning: failed adding segment %d-%d for %s (discarded)!\n",
            segstart, segend, gffID);
     delete enew;
     hasErrors(true);
     return -1;
     }
   covlen+=(int)(exons[eidx]->end-exons[eidx]->start)+1;
   //adjust parent feature coordinates to contain this exon
   if (start==0 || start>exons.First()->start) {
     start=exons.First()->start;
     }
   if (end<exons.Last()->end) end=exons.Last()->end;
   return eidx;
}

void GffObj::expandExon(int oi, uint segstart, uint segend, char exontype, double sc, char fr, int qs, int qe) {
  //oi is the index of the *first* overlapping segment found that must be enlarged
  covlen-=exons[oi]->len();
  if (segstart<exons[oi]->start)
    exons[oi]->start=segstart;
  if (qs && qs<exons[oi]->qstart) exons[oi]->qstart=qs;
  if (segend>exons[oi]->end)
    exons[oi]->end=segend;
  if (qe && qe>exons[oi]->qend) exons[oi]->qend=qe;
  //warning: score cannot be properly adjusted! e.g. if it's a p-value it's just going to get worse
  if (sc!=0) exons[oi]->score=sc;
  //covlen+=exons[oi]->len();
  //if (exons[oi]->exontype< exontype) -- always true
  exons[oi]->exontype = exontype;
  if (exontype==exgffCDS) exons[oi]->phase=fr;
  //we must check if any more exons are also overlapping this
  int ni=oi+1; //next exon index after oi
  while (ni<exons.Count() && exons[ni]->start<=segend+1) { // next segment overlaps OR adjacent to newly enlarged segment
     //if (exons[ni]->exontype<exontype && exons[ni]->end<=segend) {
	  if (exons[ni]->exontype>0) {
         if (exons[ni]->start<exons[oi]->start) exons[oi]->start=exons[ni]->start;
         if (exons[ni]->end>exons[oi]->end) exons[oi]->end=exons[ni]->end;
         if (exons[ni]->qstart<exons[oi]->qstart) exons[oi]->qstart=exons[ni]->qstart;
         if (exons[ni]->qend>exons[oi]->qend) exons[oi]->qend=exons[ni]->qend;
         exons.Delete(ni);
      } else ++ni;
      /*else {
         if (gff_show_warnings) GMessage("GFF Warning: overlapping existing exon(%d-%d) while expanding to %d-%d for GFF ID %s\n",
                exons[ni]->start, exons[ni]->end, segstart, segend, gffID);
         //hasErrors(true);
         break;
      }*/
  } //until no more overlapping/adjacent segments found
  // -- make sure any other related boundaries are updated:
  start=exons.First()->start;
  end=exons.Last()->end;
  //recalculate covlen
  covlen=0;
  for (int i=0;i<exons.Count();++i) covlen+=exons[i]->len();
  /*
  if (uptr!=NULL) { //collect stats about the underlying genomic sequence
    GSeqStat* gsd=(GSeqStat*)uptr;
    if (start<gsd->mincoord) gsd->mincoord=start;
    if (end>gsd->maxcoord) gsd->maxcoord=end;
    if (this->len()>gsd->maxfeat_len) {
        gsd->maxfeat_len=this->len();
        gsd->maxfeat=this;
        }
    }
  */
}

void GffObj::removeExon(int idx) {
  /*
   if (idx==0 && segs[0].start==gstart)
                  gstart=segs[1].start;
   if (idx==segcount && segs[segcount].end==gend)
                  gend=segs[segcount-1].end;
  */
  if (idx<0 || idx>=exons.Count()) return;
  int segstart=exons[idx]->start;
  int segend=exons[idx]->end;
  exons.Delete(idx);
  covlen -= (int)(segend-segstart)+1;
  start=exons.First()->start;
  end=exons.Last()->end;
  if (isCDS) { CDstart=start; CDend=end; }
}

void GffObj::removeExon(GffExon* p) {
	for (int idx=0;idx<exons.Count();idx++) {
		if (exons[idx]==p) {
			int segstart=exons[idx]->start;
			int segend=exons[idx]->end;
			exons.Delete(idx);
			covlen -= (int)(segend-segstart)+1;

			if (exons.Count() > 0) {
				start=exons.First()->start;
				end=exons.Last()->end;
				if (isCDS) { CDstart=start; CDend=end; }
			}
			return;
		}
	}
}

GffObj::GffObj(GffReader *gfrd, BEDLine* bedline, bool keepAttr):GSeg(0,0), exons(true,true,false) {
	xstart=0;
	xend=0;
	xstatus=0;
	partial=false;
	isCDS=false;
	uptr=NULL;
	ulink=NULL;
	parent=NULL;
	udata=0;
	flags=0;
	CDstart=0;
	CDend=0;
	CDphase=0;
	geneID=NULL;
	gene_name=NULL;
	attrs=NULL;
	gffID=NULL;
	track_id=-1;
	gseq_id=-1;
	//ftype_id=-1;
	exon_ftype_id=-1;
	strand='.';
	if (gfrd == NULL || bedline == NULL)
		GError("Error: cannot use this GffObj constructor with NULL GffReader/BEDLine!\n");
	gffnames_ref(names);
	//qlen=0;qstart=0;qend=0;
	gscore=0;
	uscore=0;
	covlen=0;
	qcov=0;
	ftype_id=gff_fid_transcript;
	exon_ftype_id=gff_fid_exon;
	start=bedline->fstart;
	end=bedline->fend;
	gseq_id=names->gseqs.addName(bedline->gseqname);
	track_id=names->tracks.addName("BED");
	strand=bedline->strand;
	qlen=0;
	qstart=0;
	qend=0;
	//setup flags from gffline
	isCDS=false;
	isGene(false);
	isTranscript(true);
	gffID=Gstrdup(bedline->ID);
	for (int i=0;i<bedline->exons.Count();++i) {
		this->addExon(bedline->exons[i].start,bedline->exons[i].end);
	}
	if (bedline->cds_start>0) {
		CDstart=bedline->cds_start;
		CDend=bedline->cds_end;
		if (CDstart>0 && bedline->cds_phase)
			CDphase=bedline->cds_phase;
	}
	if (keepAttr && bedline->info!=NULL) this->parseAttrs(attrs, bedline->info);
}

GffObj::GffObj(GffReader *gfrd, GffLine* gffline, bool keepAttr, bool noExonAttr):
     GSeg(0,0), exons(true,true,false), children(1,false) {
  xstart=0;
  xend=0;
  xstatus=0;
  partial=false;
  isCDS=false;
  uptr=NULL;
  ulink=NULL;
  parent=NULL;
  udata=0;
  flags=0;
  CDstart=0;
  CDend=0;
  CDphase=0;
  geneID=NULL;
  gene_name=NULL;
  attrs=NULL;
  gffID=NULL;
  track_id=-1;
  gseq_id=-1;
  //ftype_id=-1;
  exon_ftype_id=-1;
  strand='.';
  if (gfrd == NULL || gffline == NULL)
    GError("Cannot use this GffObj constructor with NULL GffReader/GffLine!\n");
  gffnames_ref(names);
  //qlen=0;qstart=0;qend=0;
  gscore=0;
  uscore=0;
  covlen=0;
  qcov=0;
  ftype_id=gffline->ftype_id;
  start=gffline->fstart;
  end=gffline->fend;
  gseq_id=names->gseqs.addName(gffline->gseqname);
  track_id=names->tracks.addName(gffline->track);
  strand=gffline->strand;
  qlen=gffline->qlen;
  qstart=gffline->qstart;
  qend=gffline->qend;
  //setup flags from gffline
  isCDS=gffline->is_cds; //for now
  isGene(gffline->is_gene);
  isTranscript(gffline->is_transcript || gffline->exontype!=0);
  //fromGff3(gffline->is_gff3);

  if (gffline->parents!=NULL && !gffline->is_transcript) {
    //GTF style -- create a GffObj directly by subfeature
    //(also possible orphan GFF3 exon line, or an exon given before its parent (chado))
    if (gffline->exontype!=0) { //recognized exon-like feature
       ftype_id=gff_fid_transcript; //so this is some sort of transcript
       exon_ftype_id=gff_fid_exon; //subfeatures MUST be exons
       }
     else {//unrecognized subfeatures
       //make this GffObj of the same feature type
       ftype_id=names->feats.addName(gffline->ftype);
       }
    if (gffline->ID==NULL) { //typical GTF2 without "transcript" line
        gffID=Gstrdup(gffline->parents[0]);
        this->createdByExon(true);
        //this is likely the first exon/segment of the feature
        addExon(gfrd, gffline, keepAttr, noExonAttr);
        }
      else { //a parented feature with an ID: orphan or premature GFF3 subfeature line
        if (gfrd->is_gff3 && gffline->exontype!=0) {
             //premature exon given before its parent transcript
             //create the transcript entry here
             gffID=Gstrdup(gffline->parents[0]);
             this->createdByExon(true);
             //this is the first exon/segment of the transcript
             addExon(gfrd, gffline, keepAttr, noExonAttr);
             }
        else { //unrecognized non-exon feature ? use the ID instead
             this->hasGffID(true);
             gffID=Gstrdup(gffline->ID);
             if (keepAttr) this->parseAttrs(attrs, gffline->info);
             }
        }
  } //non-transcript parented subfeature given directly
  else {
    //non-parented feature OR a recognizable transcript
    //create a parent feature in its own right
    gscore=gffline->score;
    if (gffline->ID==NULL || gffline->ID[0]==0)
      GError("Error: no ID found for GFF record start\n");
    this->hasGffID(true);
    gffID=Gstrdup(gffline->ID); //there must be an ID here
    //if (gffline->is_transcript) ftype_id=gff_fid_mRNA;
      //else
    if (gffline->is_transcript) {
      exon_ftype_id=gff_fid_exon;
      if (ftype_id<0)
        ftype_id=names->feats.addName(gffline->ftype);
      if (gfrd->is_gff3 && gffline->exons.Count()>0) {
    		for (int i=0;i<gffline->exons.Count();++i) {
    			this->addExon(gffline->exons[i].start, gffline->exons[i].end);
    		}
    		if (gffline->cds_start>0) {
    			CDstart=gffline->cds_start;
    			CDend=gffline->cds_end;
    			if (gffline->phase!=0) CDphase=gffline->phase;
    		}
      }
    } //is transcript
    if (keepAttr) this->parseAttrs(attrs, gffline->info);
    if (gfrd->is_gff3 && gffline->parents==NULL && gffline->exontype!=0) {
       //special case with bacterial genes just given as a CDS/exon, without parent!
       this->createdByExon(true);
       if (ftype_id<0) ftype_id=gff_fid_mRNA;
       addExon(gfrd, gffline, keepAttr, noExonAttr);
    }
    if (ftype_id<0)
        ftype_id=names->feats.addName(gffline->ftype);
  }//no parent OR recognizable transcript

  if (gffline->gene_name!=NULL) {
     gene_name=Gstrdup(gffline->gene_name);
     }
  if (gffline->gene_id) {
     geneID=Gstrdup(gffline->gene_id);
     }
  else if (gffline->is_transcript && gffline->parents) {
	 geneID=Gstrdup(gffline->parents[0]);
     }

  //GSeqStat* gsd=gfrd->gseqstats.AddIfNew(new GSeqStat(gseq_id,gffline->gseqname), true);
  //GSeqStat* gsd=gfrd->gseqtable[gseq_id];
  //uptr=gsd;
}

BEDLine* GffReader::nextBEDLine() {
 if (bedline!=NULL) return bedline; //caller should free gffline after processing
 while (bedline==NULL) {
	int llen=0;
	buflen=GFF_LINELEN-1;
	char* l=fgetline(linebuf, buflen, fh, &fpos, &llen);
	if (l==NULL) return NULL;
	int ns=0; //first nonspace position
	while (l[ns]!=0 && isspace(l[ns])) ns++;
	if (l[ns]=='#' || llen<7) continue;
	bedline=new BEDLine(this, l);
	if (bedline->skip) {
	  delete bedline;
	  bedline=NULL;
	  continue;
	}
 }
 return bedline;
}


GffLine* GffReader::nextGffLine() {
 if (gffline!=NULL) return gffline; //caller should free gffline after processing
 while (gffline==NULL) {
    int llen=0;
    buflen=GFF_LINELEN-1;
    char* l=fgetline(linebuf, buflen, fh, &fpos, &llen);
    if (l==NULL) {
         return NULL; //end of file
         }
#ifdef CUFFLINKS
     _crc_result.process_bytes( linebuf, llen );
#endif
    int ns=0; //first nonspace position
    while (l[ns]!=0 && isspace(l[ns])) ns++;
    if (l[ns]=='#' || llen<10) continue;
    gffline=new GffLine(this, l);
    if (gffline->skipLine) {
       delete gffline;
       gffline=NULL;
       continue;
    }
    if (gffline->ID==NULL && gffline->parents==NULL)  { //it must have an ID
        //this might not be needed, already checked in the GffLine constructor
        if (gff_warns)
            GMessage("Warning: malformed GFF line, no parent or record Id (kipping\n");
        delete gffline;
        gffline=NULL;
        //continue;
        }
    }
return gffline;
}


char* GffReader::gfoBuildId(const char* id, const char* ctg) {
//caller must free the returned pointer
 char* buf=NULL;
 int idlen=strlen(id);
 GMALLOC(buf, idlen+strlen(ctg)+2);
 strcpy(buf, id);
 buf[idlen]='~';
 strcpy(buf+idlen+1, ctg);
 return buf;
}

GffObj* GffReader::gfoAdd(GffObj* gfo) {
 GPVec<GffObj>* glst=phash.Find(gfo->gffID);
 if (glst==NULL)
	 glst=new GPVec<GffObj>(false);
 int i=glst->Add(gfo);
 phash.Add(gfo->gffID, glst);
 return glst->Get(i);
}

GffObj* GffReader::gfoAdd(GPVec<GffObj>& glst, GffObj* gfo) {
 int i=glst.Add(gfo);
 return glst[i];
}

GffObj* GffReader::gfoReplace(GPVec<GffObj>& glst, GffObj* gfo, GffObj* toreplace) {
 for (int i=0;i<glst.Count();++i) {
	 if (glst[i]==toreplace) {
		 //glst.Put(i,gfo);
		 glst[i]=gfo;
		 break;
	 }
 }
 return gfo;
}

bool GffReader::pFind(const char* id, GPVec<GffObj>*& glst) {
	glst = phash.Find(id);
	return (glst!=NULL);
}

GffObj* GffReader::gfoFind(const char* id, GPVec<GffObj>*& glst,
		const char* ctg, char strand, uint start, uint end) {
	GPVec<GffObj>* gl=NULL;
	if (glst) {
		gl=glst;
	} else {
		gl = phash.Find(id);
	}
	GffObj* gh=NULL;
	if (gl) {
		for (int i=0;i<gl->Count();i++) {
			GffObj& gfo = *(gl->Get(i));
			if (ctg!=NULL && strcmp(ctg, gfo.getGSeqName())!=0)
				continue;
			if (strand && gfo.strand!='.' && strand != gfo.strand)
				continue;
			if (start>0) {
				if (abs((int)start-(int)gfo.start)> (int)GFF_MAX_LOCUS)
					continue;
				if (end>0 && (gfo.start>end || gfo.end<start))
					continue;
			}
			//must be the same transcript, according to given comparison criteria
			gh=&gfo;
			break;
		}
	}
	if (!glst) glst=gl;
	return gh;
}

GffObj* GffReader::updateParent(GffObj* newgfo, GffObj* parent) {
  //assert(parent);
  //assert(newgfo);
  parent->children.Add(newgfo);
  if (newgfo->parent==NULL) newgfo->parent=parent;
  newgfo->setLevel(parent->getLevel()+1);
  if (parent->isGene()) {
      if (parent->gene_name!=NULL && newgfo->gene_name==NULL)
        newgfo->gene_name=Gstrdup(parent->gene_name);
      if (parent->geneID!=NULL && newgfo->geneID==NULL)
        newgfo->geneID=Gstrdup(parent->geneID);
      }

  return newgfo;
}

GffObj* GffReader::newGffRec(GffLine* gffline, bool keepAttr, bool noExonAttr,
                          GffObj* parent, GffExon* pexon, GPVec<GffObj>* glst, bool replace_parent) {
  GffObj* newgfo=new GffObj(this, gffline, keepAttr, noExonAttr);
  GffObj* r=NULL;
  gflst.Add(newgfo);
  //tag non-transcripts to be discarded later
  if (this->transcriptsOnly && this->is_gff3 && gffline->ID!=NULL &&
		  gffline->exontype==0 && !gffline->is_gene && !gffline->is_transcript) {
	  //unrecognized non-exon entity, should be discarded
	  newgfo->isDiscarded(true);
	  this->discarded_ids.Add(gffline->ID, new int(1));
  }
  if (replace_parent && glst) {
	r=gfoReplace(*glst, newgfo, parent);
	updateParent(r, parent);
  } else { //regular case of new GffObj creation
	  r=(glst) ? gfoAdd(*glst, newgfo) : gfoAdd(newgfo);
	  if (parent!=NULL) {
		updateParent(r, parent);
		if (pexon!=NULL) parent->removeExon(pexon);
	  }
  }
  return r;
}

GffObj* GffReader::newGffRec(BEDLine* bedline, bool keepAttr, GPVec<GffObj>* glst) {
  GffObj* newgfo=new GffObj(this, bedline, keepAttr);
  GffObj* r=NULL;
  gflst.Add(newgfo);
  r=(glst) ? gfoAdd(*glst, newgfo) : gfoAdd(newgfo);
  return r;
}

GffObj* GffReader::updateGffRec(GffObj* prevgfo, GffLine* gffline,
                                         bool keepAttr) {
 if (prevgfo==NULL) return NULL;
 //prevgfo->gffobj->createdByExon(false);
 if (gffline->ftype_id>=0)
	 prevgfo->ftype_id=gffline->ftype_id;
 else
	 prevgfo->ftype_id=prevgfo->names->feats.addName(gffline->ftype);
 prevgfo->start=gffline->fstart;
 prevgfo->end=gffline->fend;
 prevgfo->isGene(gffline->is_gene);
 prevgfo->isTranscript(gffline->is_transcript || gffline->exontype!=0);
 prevgfo->hasGffID(gffline->ID!=NULL);
 if (keepAttr) {
   if (prevgfo->attrs!=NULL) prevgfo->attrs->Clear();
   prevgfo->parseAttrs(prevgfo->attrs, gffline->info);
   }
 return prevgfo;
}


bool GffReader::addExonFeature(GffObj* prevgfo, GffLine* gffline, GHash<CNonExon>* pex, bool noExonAttr) {
	bool r=true;
	if (gffline->strand!=prevgfo->strand) {
		if (prevgfo->strand=='.') {
			prevgfo->strand=gffline->strand;
		}
		else {
			GMessage("GFF Error at %s (%c): exon %d-%d (%c) found on different strand; discarded.\n",
					prevgfo->gffID, prevgfo->strand, gffline->fstart, gffline->fend, gffline->strand,
					prevgfo->getGSeqName());
			return true;
		}
	}
	int gdist=(gffline->fstart>prevgfo->end) ? gffline->fstart-prevgfo->end :
			((gffline->fend<prevgfo->start)? prevgfo->start-gffline->fend :
					0 );
	if (gdist>(int)GFF_MAX_LOCUS) { //too far apart, most likely this is a duplicate ID
		GMessage("Error: duplicate GFF ID '%s' (or exons too far apart)!\n",prevgfo->gffID);
		//validation_errors = true;
		r=false;
		if (!gff_warns) exit(1);
	}
	int eidx=prevgfo->addExon(this, gffline, !noExonAttr, noExonAttr);
	if (pex!=NULL && eidx>=0) {
		//if (eidx==0 && gffline->exontype>0) prevgfo->isTranscript(true);
		if (gffline->ID!=NULL && gffline->exontype==0)
		   subfPoolAdd(*pex, prevgfo);
	}
	return r;
}

CNonExon* GffReader::subfPoolCheck(GffLine* gffline, GHash<CNonExon>& pex, char*& subp_name) {
  CNonExon* subp=NULL;
  subp_name=NULL;
  for (int i=0;i<gffline->num_parents;i++) {
    if (transcriptsOnly && discarded_ids.Find(gffline->parents[i])!=NULL)
        continue;
    subp_name=gfoBuildId(gffline->parents[i], gffline->gseqname); //e.g. mRNA name
    subp=pex.Find(subp_name);
    if (subp!=NULL)
       return subp;
    GFREE(subp_name);
    }
  return NULL;
}

void GffReader::subfPoolAdd(GHash<CNonExon>& pex, GffObj* newgfo) {
//this might become a parent feature later
if (newgfo->exons.Count()>0) {
   char* xbuf=gfoBuildId(gffline->ID, gffline->gseqname);
   pex.Add(xbuf, new CNonExon(newgfo, newgfo->exons[0], *gffline));
   GFREE(xbuf);
   }
}

GffObj* GffReader::promoteFeature(CNonExon* subp, char*& subp_name, GHash<CNonExon>& pex,
    bool keepAttr, bool noExonAttr) {
  GffObj* prevp=subp->parent; //grandparent of gffline (e.g. gene)
  //if (prevp!=gflst[subp->idx])
  //  GError("Error promoting subfeature %s, gflst index mismatch?!\n", subp->gffline->ID);
  subp->gffline->discardParent();
  GffObj* gfoh=newGffRec(subp->gffline, keepAttr, noExonAttr, prevp, subp->exon);
  pex.Remove(subp_name); //no longer a potential parent, moved it to phash already
  prevp->promotedChildren(true);
  return gfoh; //returns the holder of newly promoted feature
}


GffObj* GffReader::readNext() { //user must free the returned GffObj*
 GffObj* gfo=NULL;
 GSeg tseg(0,0);
 if (is_BED) {
	 if (nextBEDLine()) {
		 gfo=new GffObj(this, bedline);
		 tseg.start=gfo->start;
		 tseg.end=gfo->end;
		 delete bedline;
		 bedline=NULL;
	 } //else return NULL;
 } else { //GFF parsing
    while (nextGffLine()!=NULL) {
    	char* tid=gffline->ID;
    	if (gffline->is_exon) tid=gffline->parents[0];
    	else if (!gffline->is_transcript) tid=NULL; //sorry, only parsing transcript && their exons this way
    	//silly gene-only transcripts will be missed here
    	if (tid==NULL || gffline->num_parents>1) {
    		delete gffline;
    		gffline=NULL;
    		continue;
    	}
    	bool sameID=(lastReadNext!=NULL && strcmp(lastReadNext, tid)==0);
    	if (sameID) {
    		if (gfo==NULL) GError("Error: same transcript ID but GffObj inexistent?!(%s)\n", tid);
    		addExonFeature(gfo, gffline);
    	} else {
    		//new transcript
    		if (gfo==NULL) {
    			//fresh start
    			gfo=new GffObj(this, gffline);
    			if (gffline->is_transcript) {
    				tseg.start=gffline->fstart;
    				tseg.end=gffline->fend;
    			}
    		}
    		else {
    			//this gffline is for the next record
    			//return what we've got so far
    			//return gfo;
    			break;
    		}
			GFREE(lastReadNext);
			lastReadNext=Gstrdup(tid);
    	} //transcript ID change
    	//gffline processed, move on
		delete gffline;
		gffline=NULL;
    } //while nextgffline()
 } //GFF records
 if (gfo!=NULL) {
		if (gfo->exons.Count()==0 && (gfo->isTranscript() || (gfo->isGene() && this->gene2exon && gfo->children.Count()==0))) {
			gfo->addExon(gfo->start, gfo->end);
		}
	if (tseg.start>0) {
		if (tseg.start!=gfo->exons.First()->start ||
				tseg.end!=gfo->exons.Last()->end) {
			GMessage("Warning: boundary mismatch for exons of transcript %s (%d-%d) ?\n",
					gfo->getID(), gfo->start, gfo->end);
		}
	}
 }
 return gfo;
}

//have to parse the whole file because exons and other subfeatures can be scattered, unordered in the input
//Trans-splicing and fusions are only accepted in proper GFF3 format, i.e. multiple features with the same ID
//are accepted if they are NOT overlapping/continuous
//  *** BUT (exception): proximal xRNA features with the same ID, on the same strand, will be merged
//  and the segments will be treated like exons (e.g. TRNAR15 (rna1940) in RefSeq)
void GffReader::readAll(bool keepAttr, bool mergeCloseExons, bool noExonAttr) {
	bool validation_errors = false;
	if (is_BED) {
		while (nextBEDLine()) {
			GPVec<GffObj>* prevgflst=NULL;
			GffObj* prevseen=gfoFind(bedline->ID, prevgflst, bedline->gseqname, bedline->strand, bedline->fstart);
			if (prevseen) {
			//duplicate ID -- but this could also be a discontinuous feature according to GFF3 specs
			  //e.g. a trans-spliced transcript - but segments should not overlap
				if (prevseen->overlap(bedline->fstart, bedline->fend)) {
					//overlapping feature with same ID is going too far
					GMessage("Error: overlapping duplicate BED feature (ID=%s)\n", bedline->ID);
					//validation_errors = true;
					if (gff_warns) { //validation intent: just skip the feature, allow the user to see other errors
						delete bedline;
						bedline=NULL;
						continue;
					}
					else exit(1);
				}
				//create a separate entry (true discontinuous feature?)
				prevseen=newGffRec(bedline, keepAttr, prevgflst);
				if (gff_warns) {
					GMessage("Warning: duplicate BED feature ID %s (%d-%d) (discontinuous feature?)\n",
							bedline->ID, bedline->fstart, bedline->fend);
				}
			}
			else {
				newGffRec(bedline, keepAttr, prevgflst);
			}
			delete bedline;
			bedline=NULL;
		}
	}
	else { //regular GFF/GTF or perhaps TLF?
		//loc_debug=false;
		GHash<CNonExon> pex; //keep track of any parented (i.e. exon-like) features that have an ID
		//and thus could become promoted to parent features
		while (nextGffLine()!=NULL) {
			GffObj* prevseen=NULL;
			GPVec<GffObj>* prevgflst=NULL;
			if (gffline->ID && gffline->exontype==0) {
				//parent-like feature ID (mRNA, gene, etc.) not recognized as an exon feature
				//check if this ID was previously seen on the same chromosome/strand within GFF_MAX_LOCUS distance
				prevseen=gfoFind(gffline->ID, prevgflst, gffline->gseqname, gffline->strand, gffline->fstart);
				if (prevseen) {
					//same ID seen in the same locus/region
					if (prevseen->createdByExon()) {
						if (gff_show_warnings && (prevseen->start<gffline->fstart ||
								prevseen->end>gffline->fend))
							GMessage("GFF Warning: invalid coordinates for %s parent feature (ID=%s)\n", gffline->ftype, gffline->ID);
						//an exon of this ID was given before
						//this line has the main attributes for this ID
						updateGffRec(prevseen, gffline, keepAttr);
					}
					else { //possibly a duplicate ID -- but this could also be a discontinuous feature according to GFF3 specs
					    //e.g. a trans-spliced transcript - though segments should not overlap!
						bool gtf_gene_dupID=(prevseen->isGene() && gffline->is_gtf_transcript);
						if (prevseen->overlap(gffline->fstart, gffline->fend) && !gtf_gene_dupID) {
							//in some GTFs a gene ID may actually be the same with the parented transcript ID (thanks)
							//overlapping feature with same ID is going too far
							GMessage("GFF Error: overlapping duplicate %s feature (ID=%s)\n", gffline->ftype, gffline->ID);
							//validation_errors = true;
							if (gff_warns) { //validation intent: just skip the feature, allow the user to see other errors
								delete gffline;
								gffline=NULL;
								continue;
							}
							//else exit(1);
						}
						if (gtf_gene_dupID) {
							//special GTF case where parent gene_id matches transcript_id (sigh)
							prevseen=newGffRec(gffline, keepAttr, noExonAttr, prevseen, NULL, prevgflst, true);
						}
						else {
							//create a separate entry (true discontinuous feature)
							prevseen=newGffRec(gffline, keepAttr, noExonAttr,
									prevseen->parent, NULL, prevgflst);
							if (gff_warns) {
								GMessage("GFF Warning: duplicate feature ID %s (%d-%d) (discontinuous feature?)\n",
										gffline->ID, gffline->fstart, gffline->fend);
							}
						}
					} //duplicate ID in the same locus
				} //ID seen previously in the same locus
			} //parent-like ID feature (non-exon)
			if (gffline->parents==NULL) {
				//top level feature (transcript, gene), no parents (or parents can be ignored)
				if (!prevseen) newGffRec(gffline, keepAttr, noExonAttr, NULL, NULL, prevgflst);
			}
			else { //--- it's a child feature (exon/CDS or even a mRNA with a gene as parent)
				//updates all the declared parents with this child
				bool found_parent=false;
				if (gffline->is_gtf_transcript && prevseen && prevseen->parent) {
					found_parent=true; //parent already found in special GTF case
				}
				else {
					GffObj* newgfo=prevseen;
					GPVec<GffObj>* newgflst=NULL;
					GVec<int> kparents; //kept parents (non-discarded)
					GVec< GPVec<GffObj>* > kgflst(false);
					GPVec<GffObj>* gflst0=NULL;
					for (int i=0;i<gffline->num_parents;i++) {
						newgflst=NULL;
						//if (transcriptsOnly && (
						if (discarded_ids.Find(gffline->parents[i])!=NULL) continue;
						if (!pFind(gffline->parents[i], newgflst))
							continue; //skipping discarded parent feature
						kparents.Add(i);
						if (i==0) gflst0=newgflst;
						kgflst.Add(newgflst);
					}
					if (gffline->num_parents>0 && kparents.Count()==0) {
						kparents.cAdd(0);
						kgflst.Add(gflst0);
					}
					for (int k=0;k<kparents.Count();k++) {
						int i=kparents[k];
						newgflst=kgflst[k];
						GffObj* parentgfo=NULL;
						if (gffline->is_transcript || gffline->exontype==0) {//likely a transcript
							//parentgfo=gfoFind(gffline->parents[i], newgflst, gffline->gseqname,
							//		gffline->strand, gffline->fstart, gffline->fend);
							if (newgflst!=NULL && newgflst->Count()>0)
								parentgfo = newgflst->Get(0);
						}
						else {
							//for exon-like entities we only need a parent to be in locus distance,
							//on the same strand
							parentgfo=gfoFind(gffline->parents[i], newgflst, gffline->gseqname,
									gffline->strand, gffline->fstart);
						}
						if (parentgfo!=NULL) { //parent GffObj parsed earlier
							found_parent=true;
							if (parentgfo->isGene() && (gffline->is_transcript ||
									 gffline->exontype==0)) {
								//not an exon, but a transcript parented by a gene
								if (newgfo) {
									updateParent(newgfo, parentgfo);
								}
								else {
									newgfo=newGffRec(gffline, keepAttr, noExonAttr, parentgfo);
								}
							}
							else { //potential exon subfeature?
								bool addExon=false;
								if (transcriptsOnly) {
									if (gffline->exontype>0) addExon=true;
								}
								else { //always discard silly "intron" features
									if (! (gffline->exontype==exgffIntron && (parentgfo->isTranscript() || parentgfo->exons.Count()>0)))
									  addExon=true;
								}
								if (addExon)
									if (!addExonFeature(parentgfo, gffline, &pex, noExonAttr))
									   validation_errors=true;

							}
						} //overlapping parent feature found
					} //for each parsed parent Id
					if (!found_parent) { //new GTF-like record starting directly here as a subfeature
						//or it could be some chado GFF3 barf with exons coming BEFORE their parent :(
						//or it could also be a stray transcript without a parent gene defined previously
						//check if this feature isn't parented by a previously stored "child" subfeature
						char* subp_name=NULL;
						CNonExon* subp=NULL;
						if (!gffline->is_transcript) { //don't bother with this check for obvious transcripts
							if (pex.Count()>0) subp=subfPoolCheck(gffline, pex, subp_name);
							if (subp!=NULL) { //found a subfeature that is the parent of this (!)
								//promote that subfeature to a full GffObj
								GffObj* gfoh=promoteFeature(subp, subp_name, pex, keepAttr, noExonAttr);
								//add current gffline as an exon of the newly promoted subfeature
								if (!addExonFeature(gfoh, gffline, &pex, noExonAttr))
									validation_errors=true;
							}
						}
						if (subp==NULL) { //no parent subfeature seen before
							//loc_debug=true;
							GffObj* ngfo=prevseen;
							if (ngfo==NULL) {
								//if it's an exon type, create directly the parent with this exon
								//but if it's recognized as a transcript, the object itself is created
								ngfo=newGffRec(gffline, keepAttr, noExonAttr, NULL, NULL, newgflst);
							}
							if (!ngfo->isTranscript() &&
									gffline->ID!=NULL && gffline->exontype==0)
								subfPoolAdd(pex, ngfo);
							//even those with errors will be added here!
						}
						GFREE(subp_name);
					} //no previous parent found
				}
			} //parented feature
			//--
			delete gffline;
			gffline=NULL;
		}//while gff lines
	}
	if (gflst.Count()>0) {
		gflst.finalize(this, mergeCloseExons, keepAttr, noExonAttr); //force sorting by locus if so constructed
	}
	// all gff records are now loaded in GList gflst
	// so we can free the hash
	phash.Clear();
	//tids.Clear();
	if (validation_errors) {
		exit(1);
	}
}

void GfList::finalize(GffReader* gfr, bool mergeCloseExons,
             bool keepAttrs, bool noExonAttr) { //if set, enforce sort by locus
  GList<GffObj> discarded(false,true,false);
  for (int i=0;i<Count();i++) {
    //finish the parsing for each GffObj
    fList[i]->finalize(gfr, mergeCloseExons, keepAttrs, noExonAttr);
    if (fList[i]->isDiscarded()) {
       discarded.Add(fList[i]);
       if (fList[i]->children.Count()>0) {
      	 for (int c=0;c<fList[i]->children.Count();c++) {
      		 fList[i]->children[c]->parent=NULL;
      		 if (keepAttrs)
      			 fList[i]->children[c]->copyAttrs(fList[i]); //inherit the attributes of discarded parent (e.g. pseudo=true; )
      	 }
       }
       this->Forget(i);
    }
  }
  if (discarded.Count()>0) {
          this->Pack();
  }
  if (mustSort) { //force (re-)sorting
     this->setSorted(false);
     this->setSorted((GCompareProc*)gfo_cmpByLoc);
     }
}

GffObj* GffObj::finalize(GffReader* gfr, bool mergeCloseExons, bool keepAttrs, bool noExonAttr) {

 if (!isDiscarded() && exons.Count()==0 && (isTranscript() || (isGene() && children.Count()==0 && gfr->gene2exon)) ) {
		 //add exon feature to an exonless transcript
		 addExon(this->start, this->end);
		 //effectively this becomes a transcript (even the childless genes if gene2exon)
		 isTranscript(true);
 }

 if (gfr->transcriptsOnly && !isTranscript()) {
 	isDiscarded(true); //discard non-transcripts
 }

 if (ftype_id==gff_fid_transcript && CDstart>0) {
 	ftype_id=gff_fid_mRNA;
 	//exon_ftype_id=gff_fid_exon;
 }
 if (exons.Count()>0 && (isTranscript() || exon_ftype_id==gff_fid_exon)) {
 	if (mergeCloseExons) {
 		int mindist=mergeCloseExons ? 5:1;
 		for (int i=0;i<exons.Count()-1;i++) {
 			int ni=i+1;
 			uint mend=exons[i]->end;
 			while (ni<exons.Count()) {
 				int dist=(int)(exons[ni]->start-mend);
 				if (dist>mindist) break; //no merging with next segment
 				if (gfr!=NULL && gfr->gff_warns && dist!=0 && (exons[ni]->exontype!=exgffUTR && exons[i]->exontype!=exgffUTR)) {
 					GMessage("GFF warning: merging adjacent/overlapping segments of %s on %s (%d-%d, %d-%d)\n",
 							gffID, getGSeqName(), exons[i]->start, exons[i]->end,exons[ni]->start, exons[ni]->end);
 				}
 				mend=exons[ni]->end;
 				covlen-=exons[i]->len();
 				exons[i]->end=mend;
 				covlen+=exons[i]->len();
 				covlen-=exons[ni]->len();
 				if (exons[ni]->attrs!=NULL && (exons[i]->attrs==NULL ||
 						exons[i]->attrs->Count()<exons[ni]->attrs->Count())) {
 					//use the other exon attributes, if more
 					delete(exons[i]->attrs);
 					exons[i]->attrs=exons[ni]->attrs;
 					exons[ni]->attrs=NULL;
 				}
 				exons.Delete(ni);
 			} //check for merge with next exon
 		} //for each exon
 	} //merge close exons
 	//shrink transcript to the exons' span
 	this->start=exons.First()->start;
 	this->end=exons.Last()->end;
 }
 //more post processing of accepted records
 if (!this->isDiscarded()) {
 	 //attribute reduction for GTF records
 	 if (keepAttrs && !noExonAttr && !hasGffID()
 	 		&& exons.Count()>0 && exons[0]->attrs!=NULL) {
 	 	bool attrs_discarded=false;
 	 	for (int a=0;a<exons[0]->attrs->Count();a++) {
 	 		int attr_name_id=exons[0]->attrs->Get(a)->attr_id;
 	 		char* attr_name=names->attrs.getName(attr_name_id);
 	 		char* attr_val =exons[0]->attrs->Get(a)->attr_val;
 	 		bool sameExonAttr=true;
 	 		for (int i=1;i<exons.Count();i++) {
 	 			char* ov=exons[i]->getAttr(attr_name_id);
 	 			if (ov==NULL || (strcmp(ov,attr_val)!=0)) {
 	 				sameExonAttr=false;
 	 				break;
 	 			}
 	 		}
 	 		if (sameExonAttr) {
 	 			//delete this attribute from exons level
 	 			attrs_discarded=true;
 	 			this->addAttr(attr_name, attr_val);
 	 			for (int i=1;i<exons.Count();i++) {
 	 				removeExonAttr(*(exons[i]), attr_name_id);
 	 			}
 	 			exons[0]->attrs->freeItem(a);
 	 		}
 	 	}
 	 	if (attrs_discarded) exons[0]->attrs->Pack();
 	 }
 	 //FIXME: fix dubious, malformed cases of exonless, childless transcripts/genes (?)
 	 //FIXME: this is BAD for super-genes(loci) that just encompass (but not parent)
 	 //       multiple smaller genes ! (like MHC loci in NCBI annotation)

 	//collect stats for the reference genomic sequence
 	if (gfr->gseqtable.Count()<=gseq_id) {
 		gfr->gseqtable.setCount(gseq_id+1);
 	}
 	GSeqStat* gsd=gfr->gseqtable[gseq_id];
 	if (gsd==NULL) {
 		gsd=new GSeqStat(gseq_id,names->gseqs.getName(gseq_id));
 		//gfr->gseqtable.Put(gseq_id, gsd);
 		gfr->gseqtable[gseq_id]=gsd;
 		gfr->gseqStats.Add(gsd);
 	}
 	gsd->fcount++;
 	if (start<gsd->mincoord) gsd->mincoord=start;
 	if (end>gsd->maxcoord) gsd->maxcoord=end;
 	if (this->len()>gsd->maxfeat_len) {
 		gsd->maxfeat_len=this->len();
 		gsd->maxfeat=this;
 	}
 }
 uptr=NULL;
 udata=0;
 return this;
}

void GffObj::printExonList(FILE* fout) {
	//print comma delimited list of exon intervals
	for (int i=0;i<exons.Count();++i) {
		if (i>0) fprintf(fout, ",");
		fprintf(fout, "%d-%d",exons[i]->start, exons[i]->end);
	}
}

void BED_addAttribute(FILE* fout, int& acc, const char* format,... ) {
	++acc;
	if (acc==1) fprintf(fout, "\t");
	       else fprintf(fout, ";");
    va_list arguments;
    va_start(arguments,format);
    vfprintf(fout,format,arguments);
    va_end(arguments);
}

void GffObj::printBED(FILE* fout, bool cvtChars, char* dbuf, int dbuf_len) {
//print a BED-12 line + GFF3 attributes in 13th field
 int cd_start=CDstart>0? CDstart-1 : start-1;
 int cd_end=CDend>0 ? CDend : end;
 char cdphase=CDphase>0 ? CDphase : '0';
 fprintf(fout, "%s\t%d\t%d\t%s\t%d\t%c\t%d\t%d\t%c,0,0", getGSeqName(), start-1, end, getID(),
		 100, strand, cd_start, cd_end, cdphase);
 if (exons.Count()>0) {
	 int i;
	 fprintf(fout, "\t%d\t", exons.Count());
	 for (i=0;i<exons.Count();++i)
		 fprintf(fout,"%d,",exons[i]->len());
	 fprintf(fout, "\t");
	 for (i=0;i<exons.Count();++i)
		 fprintf(fout,"%d,",exons[i]->start-start);
 } else { //no-exon feature(!), shouldn't happen
	 fprintf(fout, "\t1\t%d,\t0,", len());
 }
 //now add the GFF3 attributes for in the 13th field
 int numattrs=0;
 if (CDstart>0) BED_addAttribute(fout, numattrs,"CDS=%d:%d",CDstart-1, CDend);
 if (CDphase>0) BED_addAttribute(fout, numattrs,"CDSphase=%c", CDphase);
 if (geneID!=NULL)
	 BED_addAttribute(fout, numattrs, "geneID=%s",geneID);
 if (gene_name!=NULL)
    fprintf(fout, ";gene_name=%s",gene_name);
 if (attrs!=NULL) {
    for (int i=0;i<attrs->Count();i++) {
      const char* attrname=names->attrs.getName(attrs->Get(i)->attr_id);
      const char* attrval=attrs->Get(i)->attr_val;
      if (attrval==NULL || attrval[0]=='\0') {
    	  BED_addAttribute(fout, numattrs,"%s",attrname);
    	  continue;
      }
      if (cvtChars) {
    	  decodeHexChars(dbuf, attrval, dbuf_len-1);
    	  BED_addAttribute(fout, numattrs, "%s=%s", attrname, dbuf);
      }
      else
    	  BED_addAttribute(fout, numattrs,"%s=%s", attrname, attrs->Get(i)->attr_val);
    }
 }
 fprintf(fout, "\n");
}

void GffObj::parseAttrs(GffAttrs*& atrlist, char* info, bool isExon) {
  if (names==NULL)
     GError(ERR_NULL_GFNAMES, "parseAttrs()");
  if (atrlist==NULL)
      atrlist=new GffAttrs();
  char* endinfo=info+strlen(info);
  char* start=info;
  char* pch=start;
  while (start<endinfo) {
    //skip spaces
    while (*start==' ' && start<endinfo) start++;
    pch=strchr(start, ';');
    if (pch==NULL) pch=endinfo;
       else {
            *pch='\0';
            pch++;
            }
    char* ech=strchr(start,'=');
    if (ech!=NULL) { // attr=value format found
       *ech='\0';
       //if (noExonAttr && (strcmp(start, "exon_number")==0 || strcmp(start, "exon")==0)) { start=pch; continue; }
       if (strcmp(start, "exon_number")==0 || strcmp(start, "exon")==0 ||
              strcmp(start, "exon_id")==0)
           { start=pch; continue; }
       ech++;
       while (*ech==' ' && ech<endinfo) ech++;//skip extra spaces after the '='
       //atrlist->Add(new GffAttr(names->attrs.addName(start),ech));
       //make sure we don't add the same attribute more than once
       if (isExon && (strcmp(start, "protein_id")==0)) {
             //Ensembl special case
             this->addAttr(start, ech);
             start=pch;
             continue;
             }
       atrlist->add_or_update(names, start, ech);
       }
      /*
      else { //not an attr=value format
        atrlist->Add(new GffAttr(names->attrs.addName(start),"1"));
        }
      */
    start=pch;
    }
  if (atrlist->Count()==0) { delete atrlist; atrlist=NULL; }
}

void GffObj::addAttr(const char* attrname, const char* attrvalue) {
  if (this->attrs==NULL)
      this->attrs=new GffAttrs();
  //this->attrs->Add(new GffAttr(names->attrs.addName(attrname),attrvalue));
  this->attrs->add_or_update(names, attrname, attrvalue);
}

void GffObj::copyAttrs(GffObj* from) { //typically from is the parent gene, and this is a transcript
	if (from==NULL || from->attrs==NULL) return;
	if (this->attrs==NULL) {
		this->attrs=new GffAttrs();
	}
	//special RefSeq case
	int desc_attr_id=names->attrs.getId("description"); //from gene
	int prod_attr_id=names->attrs.getId("product"); //from transcript (this)
	char* prod = (prod_attr_id>=0) ? this->attrs->getAttr(prod_attr_id) : NULL;

	for (int i=0;i<from->attrs->Count();++i) {
		//this->attrs->add_no_update(names, from->attrs->Get(i)->attr_id, from->attrs->Get(i)->attr_val);
		int aid=from->attrs->Get(i)->attr_id;
		//special case for GenBank refseq genes vs transcripts:
		if (prod && aid==desc_attr_id && strcmp(from->attrs->getAttr(desc_attr_id), prod)==0)
			continue; //skip description if product already there and the same
		bool haveit=false;
		for (int ai=0;ai<this->attrs->Count();++ai) {
			//do we have it already?
			if (aid==this->attrs->Get(ai)->attr_id) {
				haveit=true;
				break; //skip this, don't replace
			}
		}
		if (!haveit)
			this->attrs->Add(new GffAttr(aid, from->attrs->Get(i)->attr_val));
	}
}

void GffObj::setFeatureName(const char* feature) {
 //change the feature name/type for a transcript
 int fid=names->feats.addName(feature);
 if (monoFeature() && exons.Count()>0)
    this->exon_ftype_id=fid;
 this->ftype_id=fid;
}

void GffObj::setRefName(const char* newname) {
 //change the feature name/type for a transcript
 int rid=names->gseqs.addName(newname);
 this->gseq_id=rid;
}



int GffObj::removeAttr(const char* attrname, const char* attrval) {
  if (this->attrs==NULL || attrname==NULL || attrname[0]==0) return 0;
  int aid=this->names->attrs.getId(attrname);
  if (aid<0) return 0;
  int delcount=0;  //could be more than one ?
  for (int i=0;i<this->attrs->Count();i++) {
     if (aid==this->attrs->Get(i)->attr_id) {
       if (attrval==NULL ||
          strcmp(attrval, this->attrs->Get(i)->attr_val)==0) {
             delcount++;
             this->attrs->freeItem(i);
             }
       }
     }
  if (delcount>0) this->attrs->Pack();
  return delcount;
}

int GffObj::removeAttr(int aid, const char* attrval) {
  if (this->attrs==NULL || aid<0) return 0;
  int delcount=0;  //could be more than one ?
  for (int i=0;i<this->attrs->Count();i++) {
     if (aid==this->attrs->Get(i)->attr_id) {
       if (attrval==NULL ||
          strcmp(attrval, this->attrs->Get(i)->attr_val)==0) {
             delcount++;
             this->attrs->freeItem(i);
             }
       }
     }
  if (delcount>0) this->attrs->Pack();
  return delcount;
}


int GffObj::removeExonAttr(GffExon& exon, const char* attrname, const char* attrval) {
  if (exon.attrs==NULL || attrname==NULL || attrname[0]==0) return 0;
  int aid=this->names->attrs.getId(attrname);
  if (aid<0) return 0;
  int delcount=0;  //could be more than one
  for (int i=0;i<exon.attrs->Count();i++) {
     if (aid==exon.attrs->Get(i)->attr_id) {
       if (attrval==NULL ||
          strcmp(attrval, exon.attrs->Get(i)->attr_val)==0) {
             delcount++;
             exon.attrs->freeItem(i);
             }
       }
     }
  if (delcount>0) exon.attrs->Pack();
  return delcount;
}

int GffObj::removeExonAttr(GffExon& exon, int aid, const char* attrval) {
  if (exon.attrs==NULL || aid<0) return 0;
  int delcount=0;  //could be more than one
  for (int i=0;i<exon.attrs->Count();i++) {
     if (aid==exon.attrs->Get(i)->attr_id) {
       if (attrval==NULL ||
          strcmp(attrval, exon.attrs->Get(i)->attr_val)==0) {
             delcount++;
             exon.attrs->freeItem(i);
             }
       }
     }
  if (delcount>0) exon.attrs->Pack();
  return delcount;
}


void GffObj::getCDS_ends(uint& cds_start, uint& cds_end) {
  cds_start=0;
  cds_end=0;
  if (CDstart==0 || CDend==0) return; //no CDS info
  int cdsadj=0;
  if (CDphase=='1' || CDphase=='2') {
      cdsadj=CDphase-'0';
      }
  cds_start=CDstart;
  cds_end=CDend;
  if (strand=='-') cds_end-=cdsadj;
              else cds_start+=cdsadj;
  }

void GffObj::mRNA_CDS_coords(uint& cds_mstart, uint& cds_mend) {
  //sets cds_start and cds_end to the CDS start,end coordinates on the spliced mRNA transcript
  cds_mstart=0;
  cds_mend=0;
  if (CDstart==0 || CDend==0) return; //no CDS info
  //restore normal coordinates, just in case
  unxcoord();
  int cdsadj=0;
  if (CDphase=='1' || CDphase=='2') {
      cdsadj=CDphase-'0';
      }
  /*
   uint seqstart=CDstart;
   uint seqend=CDend;
  */
  uint seqstart=exons.First()->start;
  uint seqend=exons.Last()->end;
  int s=0; //resulting nucleotide counter
  if (strand=='-') {
    for (int x=exons.Count()-1;x>=0;x--) {
       uint sgstart=exons[x]->start;
       uint sgend=exons[x]->end;
       if (seqend<sgstart || seqstart>sgend) continue;
       if (seqstart>=sgstart && seqstart<=sgend)
             sgstart=seqstart; //seqstart within this segment
       if (seqend>=sgstart && seqend<=sgend)
             sgend=seqend; //seqend within this segment
       s+=(int)(sgend-sgstart)+1;
       if (CDstart>=sgstart && CDstart<=sgend) {
             //CDstart in this segment
             //and we are getting the whole transcript
             cds_mend=s-(int)(CDstart-sgstart);
             }
       if (CDend>=sgstart && CDend<=sgend) {
             //CDstart in this segment
             //and we are getting the whole transcript
             cds_mstart=s-(int)(CDend-cdsadj-sgstart);
             }
      } //for each exon
    } // - strand
   else { // + strand
    for (int x=0;x<exons.Count();x++) {
      uint sgstart=exons[x]->start;
      uint sgend=exons[x]->end;
      if (seqend<sgstart || seqstart>sgend) continue;
      if (seqstart>=sgstart && seqstart<=sgend)
            sgstart=seqstart; //seqstart within this segment
      if (seqend>=sgstart && seqend<=sgend)
            sgend=seqend; //seqend within this segment
      s+=(int)(sgend-sgstart)+1;
      /* for (uint i=sgstart;i<=sgend;i++) {
          spliced[s]=gsubseq[i-gstart];
          s++;
          }//for each nt
          */
      if (CDstart>=sgstart && CDstart<=sgend) {
            //CDstart in this segment
            cds_mstart=s-(int)(sgend-CDstart-cdsadj);
            }
      if (CDend>=sgstart && CDend<=sgend) {
            //CDend in this segment
            cds_mend=s-(int)(sgend-CDend);
            }
      } //for each exon
    } // + strand
  //spliced[s]=0;
  //if (rlen!=NULL) *rlen=s;
  //return spliced;
}

char* GffObj::getUnspliced(GFaSeqGet* faseq, int* rlen, GList<GSeg>* seglst)
{
    if (faseq==NULL) { GMessage("Warning: getUnspliced(NULL,.. ) called!\n");
        return NULL;
    }
    //restore normal coordinates:
    unxcoord();
    if (exons.Count()==0) return NULL;
    int fspan=end-start+1;
    const char* gsubseq=faseq->subseq(start, fspan);
    if (gsubseq==NULL) {
        GError("Error getting subseq for %s (%d..%d)!\n", gffID, start, end);
    }
    char* unspliced=NULL;

    int seqstart=exons.First()->start;
    int seqend=exons.Last()->end;

    int unsplicedlen = 0;

    unsplicedlen += seqend - seqstart + 1;

    GMALLOC(unspliced, unsplicedlen+1); //allocate more here
    //uint seqstart, seqend;

    int s = 0; //resulting nucleotide counter
    if (strand=='-')
    {
        if (seglst!=NULL)
            seglst->Add(new GSeg(s+1,s+1+seqend-seqstart));
        for (int i=seqend;i>=seqstart;i--)
        {
            unspliced[s] = ntComplement(gsubseq[i-start]);
            s++;
        }//for each nt
    } // - strand
    else
    { // + strand
        if (seglst!=NULL)
            seglst->Add(new GSeg(s+1,s+1+seqend-seqstart));
        for (int i=seqstart;i<=seqend;i++)
        {
            unspliced[s]=gsubseq[i-start];
            s++;
        }//for each nt
    } // + strand
    //assert(s <= unsplicedlen);
    unspliced[s]=0;
    if (rlen!=NULL) *rlen=s;
    return unspliced;
}

char* GffObj::getSpliced(GFaSeqGet* faseq, bool CDSonly, int* rlen, uint* cds_start, uint* cds_end,
          GList<GSeg>* seglst) {
  if (CDSonly && CDstart==0) return NULL;
  if (faseq==NULL) {
	  GMessage("Warning: getSpliced(NULL,.. ) called!\n");
      return NULL;
  }
  //restore normal coordinates:
  unxcoord();
  if (exons.Count()==0) return NULL;
  int fspan=end-start+1;
  const char* gsubseq=faseq->subseq(start, fspan);
  if (gsubseq==NULL) {
        GError("Error getting subseq for %s (%d..%d)!\n", gffID, start, end);
  }
  if (fspan<(int)(end-start+1)) { //special case: stop coordinate was extended past the gseq length, must adjust
     int endadj=end-start+1-fspan;
     uint prevend=end;
     end-=endadj;
     if (CDend>end) CDend=end;
     if (exons.Last()->end>end) {
         exons.Last()->end=end; //this could get us into trouble if exon start is also > end
         if (exons.Last()->start>exons.Last()->end) {
            GError("GffObj::getSpliced() error: improper genomic coordinate %d on %s for %s\n",
                  prevend,getGSeqName(), getID());
            }
         covlen-=endadj;
         }
     }
  char* spliced=NULL;
  GMALLOC(spliced, covlen+1); //allocate more here
  uint seqstart, seqend;
  int cdsadj=0;
  if (CDphase=='1' || CDphase=='2') {
      cdsadj=CDphase-'0';
      }
  if (CDSonly) {
     seqstart=CDstart;
     seqend=CDend;
     if (strand=='-') seqend-=cdsadj;
           else seqstart+=cdsadj;
     if (seqend-seqstart<3)
    	 GMessage("Warning: CDS %d-%d too short for %s, check your data.\n",
    			 seqstart, seqend, gffID);
     }
   else {
     seqstart=exons.First()->start;
     seqend=exons.Last()->end;
     }
  int s=0; //resulting nucleotide counter
  if (strand=='-') {
    for (int x=exons.Count()-1;x>=0;x--) {
       uint sgstart=exons[x]->start;
       uint sgend=exons[x]->end;
       if (seqend<sgstart || seqstart>sgend) continue;
       if (seqstart>=sgstart && seqstart<=sgend)
             sgstart=seqstart; //seqstart within this segment
       if (seqend>=sgstart && seqend<=sgend)
             sgend=seqend; //seqend within this segment
       if (seglst!=NULL)
           seglst->Add(new GSeg(s+1,s+1+sgend-sgstart));
       for (uint i=sgend;i>=sgstart;i--) {
            spliced[s] = ntComplement(gsubseq[i-start]);
            s++;
            }//for each nt

       if (!CDSonly && cds_start!=NULL && CDstart>0) {
          if (CDstart>=sgstart && CDstart<=sgend) {
             //CDstart in this segment
             //and we are getting the whole transcript
             *cds_end=s-(CDstart-sgstart);
             }
          if (CDend>=sgstart && CDend<=sgend) {
             //CDstart in this segment
             //and we are getting the whole transcript
             *cds_start=s-(CDend-cdsadj-sgstart);
             }
         }//update local CDS coordinates
      } //for each exon
    } // - strand
   else { // + strand
    for (int x=0;x<exons.Count();x++) {
      uint sgstart=exons[x]->start;
      uint sgend=exons[x]->end;
      if (seqend<sgstart || seqstart>sgend) continue;
      if (seqstart>=sgstart && seqstart<=sgend)
            sgstart=seqstart; //seqstart within this segment
      if (seqend>=sgstart && seqend<=sgend)
            sgend=seqend; //seqend within this segment
      if (seglst!=NULL)
          seglst->Add(new GSeg(s+1,s+1+sgend-sgstart));
      for (uint i=sgstart;i<=sgend;i++) {
          spliced[s]=gsubseq[i-start];
          s++;
          }//for each nt
      if (!CDSonly && cds_start!=NULL && CDstart>0) {
         if (CDstart>=sgstart && CDstart<=sgend) {
            //CDstart in this segment
            //and we are getting the whole transcript
            *cds_start=s-(sgend-CDstart-cdsadj);
            }
         if (CDend>=sgstart && CDend<=sgend) {
            //CDstart in this segment
            //and we are getting the whole transcript
            *cds_end=s-(sgend-CDend);
            }
        }//update local CDS coordinates
      } //for each exon
    } // + strand
  spliced[s]=0;
  if (rlen!=NULL) *rlen=s;
  return spliced;
}

char* GffObj::getSplicedTr(GFaSeqGet* faseq, bool CDSonly, int* rlen) {
  if (CDSonly && CDstart==0) return NULL;
  //restore normal coordinates:
  unxcoord();
  if (exons.Count()==0) return NULL;
  int fspan=end-start+1;
  const char* gsubseq=faseq->subseq(start, fspan);
  if (gsubseq==NULL) {
    GError("Error getting subseq for %s (%d..%d)!\n", gffID, start, end);
    }

  char* translation=NULL;
  GMALLOC(translation, (int)(covlen/3)+1);
  uint seqstart, seqend;
  int cdsadj=0;
  if (CDphase=='1' || CDphase=='2') {
      cdsadj=CDphase-'0';
      }
  if (CDSonly) {
     seqstart=CDstart;
     seqend=CDend;
     if (strand=='-') seqend-=cdsadj;
           else seqstart+=cdsadj;
     }
   else {
     seqstart=exons.First()->start;
     seqend=exons.Last()->end;
     }
  Codon codon;
  int nt=0; //codon nucleotide counter (0..2)
  int aa=0; //aminoacid count
  if (strand=='-') {
    for (int x=exons.Count()-1;x>=0;x--) {
       uint sgstart=exons[x]->start;
       uint sgend=exons[x]->end;
       if (seqend<sgstart || seqstart>sgend) continue;
       if (seqstart>=sgstart && seqstart<=sgend)
             sgstart=seqstart; //seqstart within this segment
       if (seqend>=sgstart && seqend<=sgend) {
             sgend=seqend; //seqend within this segment
             }
       for (uint i=sgend;i>=sgstart;i--) {
            codon.nuc[nt]=ntComplement(gsubseq[i-start]);
            nt++;
            if (nt==3) {
               nt=0;
               translation[aa]=codon.translate();
               aa++;
               }
            }//for each nt
      } //for each exon
    } // - strand
   else { // + strand
    for (int x=0;x<exons.Count();x++) {
      uint sgstart=exons[x]->start;
      uint sgend=exons[x]->end;
      if (seqend<sgstart || seqstart>sgend) continue;
      if (seqstart>=sgstart && seqstart<=sgend)
            sgstart=seqstart; //seqstart within this segment
      if (seqend>=sgstart && seqend<=sgend)
            sgend=seqend; //seqend within this segment
      for (uint i=sgstart;i<=sgend;i++) {
          codon.nuc[nt]=gsubseq[i-start];
          nt++;
          if (nt==3) {
             nt=0;
             translation[aa]=codon.translate();
             aa++;
             }
          }//for each nt
        } //for each exon
    } // + strand
 translation[aa]=0;
 if (rlen!=NULL) *rlen=aa;
 return translation;
}

void GffObj::printSummary(FILE* fout) {
 if (fout==NULL) fout=stdout;
 fprintf(fout, "%s\t%c\t%d\t%d\t%4.2f\t%4.1f\n", gffID,
          strand, start, end, gscore, (float)qcov/10.0);
}
//TODO we should also have an escapeChars function for when
//we want to write a GFF3 strictly compliant to the dang specification
void GffObj::decodeHexChars(char* dbuf, const char* s, int maxlen) {
	int dlen=0;
	dbuf[0]=0;
	if (s==NULL) return;
	for (const char* p=s;(*p)!=0 && dlen<maxlen;++p) {
		if (p[0]=='%' && isxdigit(p[1]) && isxdigit(p[2])) {
			int a=p[1];
			if (a>'Z') a^=0x20; //toupper()
			if (a>'9') a=10+(a-'A');
			      else a-='0';
			int b=p[2];
			if (b>'Z') b^=0x20;
			if (b>'9') b=10+(b-'A');
			      else b-='0';
			char c=(char)((a<<4)+b);
			if (c==';') c='.';
			if (c<='\t') c=' ';
			if (c>=' ') {
				dbuf[dlen]=c;
				++p;++p;
				++dlen;
				continue;
			}
		}
		dbuf[dlen]=*p;
		++dlen;
	}
	dbuf[dlen]=0;
}

void GffObj::printGTab(FILE* fout, char** extraAttrs) {
	fprintf(fout, "%s\t%c\t%d\t%d\t%s\t", this->getGSeqName(), this->strand,
			this->start, this->end, this->getID());
	if (exons.Count()) printExonList(fout);
	else fprintf(fout, ".");
	if (extraAttrs!=NULL) {
		//print a list of "attr=value;" pairs here as the last column
		//for requested attributes
		bool t1=true;
		for (int i=0;extraAttrs[i]!=NULL;++i) {
			const char* v=this->getAttr(extraAttrs[i]);
			if (v==NULL) continue;
			if (t1) { fprintf(fout, "\t"); t1=false; }
			fprintf(fout, "%s=%s;", extraAttrs[i], v);
		}
	}
	fprintf(fout,"\n");
}

void GffObj::printGxfLine(FILE* fout, const char* tlabel, const char* gseqname, bool iscds,
                             uint segstart, uint segend, int exidx,
							 char phase, bool gff3, bool cvtChars,
							 char* dbuf, int dbuf_len) {
  strcpy(dbuf,".");
  GffAttrs* xattrs=NULL;
  if (exidx>=0) {
     if (exons[exidx]->score) sprintf(dbuf,"%.2f", exons[exidx]->score);
     xattrs=exons[exidx]->attrs;
  }
  if (phase==0 || !iscds) phase='.';
  const char* ftype=iscds ? "CDS" : getSubfName();
  const char* attrname=NULL;
  const char* attrval=NULL;
  if (gff3) {
    fprintf(fout,
      "%s\t%s\t%s\t%d\t%d\t%s\t%c\t%c\tParent=%s",
      gseqname, tlabel, ftype, segstart, segend, dbuf, strand,
      phase, gffID);
    if (xattrs!=NULL) {
      for (int i=0;i<xattrs->Count();i++) {
        attrname=names->attrs.getName(xattrs->Get(i)->attr_id);
        if (cvtChars) {
          decodeHexChars(dbuf, xattrs->Get(i)->attr_val, dbuf_len-1);
          fprintf(fout,";%s=%s", attrname, dbuf);
        } else {
          fprintf(fout,";%s=%s", attrname, xattrs->Get(i)->attr_val);
        }
      }
    }
    fprintf(fout, "\n");
    } //GFF3
  else {//for GTF -- we print only transcripts
    //if (isValidTranscript())
    fprintf(fout, "%s\t%s\t%s\t%d\t%d\t%s\t%c\t%c\ttranscript_id \"%s\";",
           gseqname, tlabel, ftype, segstart, segend, dbuf, strand, phase, gffID);
    //char* geneid=(geneID!=NULL)? geneID : gffID;
    if (geneID)
      fprintf(fout," gene_id \"%s\";",geneID);
    if (gene_name!=NULL) {
       //fprintf(fout, " gene_name ");
       //if (gene_name[0]=='"') fprintf (fout, "%s;",gene_name);
       //              else fprintf(fout, "\"%s\";",gene_name);
       fprintf(fout," gene_name \"%s\";",gene_name);
       }
    if (xattrs!=NULL) {
          for (int i=0;i<xattrs->Count();i++) {
            if (xattrs->Get(i)->attr_val==NULL) continue;
            attrname=names->attrs.getName(xattrs->Get(i)->attr_id);
            fprintf(fout, " %s ",attrname);
            if (cvtChars) {
              decodeHexChars(dbuf, xattrs->Get(i)->attr_val, dbuf_len-1);
              attrval=dbuf;
            } else {
              attrval=xattrs->Get(i)->attr_val;
            }

            if (attrval[0]=='"') fprintf(fout, "%s;",attrval);
                           else fprintf(fout, "\"%s\";",attrval);
             }
          }
    //for GTF, also append the GffObj attributes to each exon line
    if ((xattrs=this->attrs)!=NULL) {
          for (int i=0;i<xattrs->Count();i++) {
            if (xattrs->Get(i)->attr_val==NULL) continue;
            attrname=names->attrs.getName(xattrs->Get(i)->attr_id);
            fprintf(fout, " %s ",attrname);
            if (cvtChars) {
              decodeHexChars(dbuf, xattrs->Get(i)->attr_val, dbuf_len-1);
              attrval=dbuf;
            } else {
              attrval=xattrs->Get(i)->attr_val;
            }
            if (attrval[0]=='"') fprintf(fout, "%s;",attrval);
                           else fprintf(fout, "\"%s\";",attrval);
             }
           }
    fprintf(fout, "\n");
    }//GTF
}

void GffObj::printGxf(FILE* fout, GffPrintMode gffp,
                   const char* tlabel, const char* gfparent, bool cvtChars) {
 const int DBUF_LEN=1024;
 char dbuf[DBUF_LEN];
 if (tlabel==NULL) {
    tlabel=track_id>=0 ? names->tracks.Get(track_id)->name :
         (char*)"gffobj" ;
    }
 unxcoord();
 if (gffp==pgffBED) {
	 printBED(fout, cvtChars, dbuf, DBUF_LEN);
	 return;
 }
 const char* gseqname=names->gseqs.Get(gseq_id)->name;
 bool gff3 = (gffp>=pgffAny && gffp<=pgffTLF);
 bool showCDS = (gffp==pgtfAny || gffp==pgtfCDS || gffp==pgffCDS || gffp==pgffAny || gffp==pgffBoth);
 bool showExon = (gffp<=pgtfExon || gffp==pgffAny || gffp==pgffExon || gffp==pgffBoth);
 if (gff3) {
   //print GFF3 transcript line:
   if (gscore>0.0) sprintf(dbuf,"%.2f", gscore);
          else strcpy(dbuf,".");
   uint pstart, pend;
   if (gffp==pgffCDS) {
      pstart=CDstart;
      pend=CDend;
      }
   else { pstart=start;pend=end; }
   //const char* ftype=isTranscript() ? "mRNA" : getFeatureName();
   const char* ftype=getFeatureName();
   fprintf(fout,
     "%s\t%s\t%s\t%d\t%d\t%s\t%c\t.\tID=%s",
     gseqname, tlabel, ftype, pstart, pend, dbuf, strand, gffID);
   if (gfparent!=NULL && gffp!=pgffTLF) {
      //parent override
      fprintf(fout, ";Parent=%s",gfparent);
   }
   else {
      if (parent!=NULL && !parent->isDiscarded() && gffp!=pgffTLF)
           fprintf(fout, ";Parent=%s",parent->getID());
   }
   if (gffp==pgffTLF) {
	   fprintf(fout, ";exonCount=%d",exons.Count());
	   if (exons.Count()>0)
		   fprintf(fout, ";exons=%d-%d", exons[0]->start, exons[0]->end);
	   for (int i=1;i<exons.Count();++i) {
		   fprintf(fout, ",%d-%d",exons[i]->start, exons[i]->end);
	   }
   }
   if (CDstart>0 && !showCDS) fprintf(fout,";CDS=%d:%d",CDstart,CDend);
   if (CDphase>0 && !showCDS) fprintf(fout,";CDSphase=%c", CDphase);
   if (geneID!=NULL)
      fprintf(fout, ";geneID=%s",geneID);
   if (gene_name!=NULL)
      fprintf(fout, ";gene_name=%s",gene_name);
   if (attrs!=NULL) {
	    for (int i=0;i<attrs->Count();i++) {
	      const char* attrname=names->attrs.getName(attrs->Get(i)->attr_id);
	      const char* attrval=attrs->Get(i)->attr_val;
	      if (attrval==NULL || attrval[0]=='\0') {
	    	  fprintf(fout,";%s",attrname);
	    	  continue;
	      }
	      if (cvtChars) {
	    	  decodeHexChars(dbuf, attrval, DBUF_LEN-1);
	    	  fprintf(fout,";%s=%s", attrname, dbuf);
	      }
	      else
	    	 fprintf(fout,";%s=%s", attrname, attrs->Get(i)->attr_val);
	    }
   }
   fprintf(fout,"\n");
 }// gff3 transcript line
 if (gffp==pgffTLF) return;
 bool is_cds_only = (gffp==pgffBoth) ? false : isCDS;
 if (showExon) {
   //print exons
    if (isCDS && exons.Count()>0 &&
        ((strand=='-' && exons.Last()->phase<'0') || (strand=='+' && exons.Last()->phase<'0')))
         updateExonPhase();
    for (int i=0;i<exons.Count();i++) {
      printGxfLine(fout, tlabel, gseqname, is_cds_only, exons[i]->start,
    		  exons[i]->end, i, exons[i]->phase, gff3, cvtChars, dbuf, DBUF_LEN);
      }
 }//printing exons
 if (showCDS && !is_cds_only && CDstart>0) {
	  if (isCDS) {
	    for (int i=0;i<exons.Count();i++) {
	      printGxfLine(fout, tlabel, gseqname, true,
	    		  exons[i]->start, exons[i]->end, i, exons[i]->phase, gff3, cvtChars,
				  dbuf, DBUF_LEN);
	    }
	  } //just an all-CDS transcript
	  else { //regular CDS printing
			GArray<GffCDSeg> cds(true,true);
			getCDSegs(cds);
			for (int i=0;i<cds.Count();i++) {
				printGxfLine(fout, tlabel, gseqname, true, cds[i].start, cds[i].end, -1,
						cds[i].phase, gff3, cvtChars, dbuf, DBUF_LEN);
			}
	  }
  } //showCDS
}

void GffObj::updateExonPhase() {
  if (!isCDS) return;
  int cdsacc=0;
  if (CDphase=='1' || CDphase=='2') {
      cdsacc+= 3-(CDphase-'0');
      }
  if (strand=='-') { //reverse strand
     for (int i=exons.Count()-1;i>=0;i--) {
         exons[i]->phase='0'+ (3-cdsacc%3)%3;
         cdsacc+=exons[i]->end-exons[i]->start+1;
         }
     }
    else { //forward strand
     for (int i=0;i<exons.Count();i++) {
         exons[i]->phase='0'+ (3-cdsacc%3)%3;
         cdsacc+=exons[i]->end-exons[i]->start+1;
         }
     }
}


void GffObj::getCDSegs(GArray<GffCDSeg>& cds) {
  GffCDSeg cdseg;
  int cdsacc=0;
  if (CDphase=='1' || CDphase=='2') {
      cdsacc+= 3-(CDphase-'0');
      }
  if (strand=='-') {
     for (int x=exons.Count()-1;x>=0;x--) {
        uint sgstart=exons[x]->start;
        uint sgend=exons[x]->end;
        if (CDend<sgstart || CDstart>sgend) continue;
        if (CDstart>=sgstart && CDstart<=sgend)
              sgstart=CDstart; //cdstart within this segment
        if (CDend>=sgstart && CDend<=sgend)
              sgend=CDend; //cdend within this segment
        cdseg.start=sgstart;
        cdseg.end=sgend;
        cdseg.exonidx=x;
        //cdseg.phase='0'+(cdsacc>0 ? (3-cdsacc%3)%3 : 0);
        cdseg.phase='0'+ (3-cdsacc%3)%3;
        cdsacc+=sgend-sgstart+1;
        cds.Add(cdseg);
       } //for each exon
     } // - strand
    else { // + strand
     for (int x=0;x<exons.Count();x++) {
       uint sgstart=exons[x]->start;
       uint sgend=exons[x]->end;
       if (CDend<sgstart || CDstart>sgend) continue;
       if (CDstart>=sgstart && CDstart<=sgend)
             sgstart=CDstart; //seqstart within this segment
       if (CDend>=sgstart && CDend<=sgend)
             sgend=CDend; //seqend within this segment
       cdseg.start=sgstart;
       cdseg.end=sgend;
       cdseg.exonidx=x;
       //cdseg.phase='0'+(cdsacc>0 ? (3-cdsacc%3)%3 : 0);
       cdseg.phase='0' + (3-cdsacc%3)%3 ;
       cdsacc+=sgend-sgstart+1;
       cds.Add(cdseg);
       } //for each exon
   } // + strand
}

//-- transcript overlap classification functions

bool singleExonTMatch(GffObj& m, GffObj& r, int& ovlen) {
 //if (m.exons.Count()>1 || r.exons.Count()>1..)
 GSeg mseg(m.start, m.end);
 ovlen=mseg.overlapLen(r.start,r.end);
 // fuzzy matching for single-exon transcripts:
 // overlap should be 80% of the length of the longer one
 if (m.covlen>r.covlen) {
   return ( (ovlen >= m.covlen*0.8) ||
		   (ovlen >= r.covlen*0.8 && ovlen >= m.covlen* 0.7 ));
		   //allow also some fuzzy reverse containment
 } else
   return (ovlen >= r.covlen*0.8);
}

//formerly in gffcompare
char getOvlCode(GffObj& m, GffObj& r, int& ovlen) {
	ovlen=0; //total actual exonic overlap
	if (!m.overlap(r.start,r.end)) return 0;
	int jmax=r.exons.Count()-1;
	//int iovlen=0; //total m.exons overlap with ref introns
	char rcode=0;
	if (m.exons.Count()==1) { //single-exon transfrag
		GSeg mseg(m.start, m.end);
		if (jmax==0) { //also single-exon ref
			//ovlen=mseg.overlapLen(r.start,r.end);
			if (singleExonTMatch(m, r, ovlen))
				return '=';
			if (m.covlen<r.covlen)
			   { if (ovlen >= m.covlen*0.8) return 'c'; } // fuzzy containment
			else
				if (ovlen >= r.covlen*0.8 ) return 'k';   // fuzzy reverse containment
			return 'o'; //just plain overlapping
		}
		//-- single-exon qry overlaping multi-exon ref
		//check full pre-mRNA case (all introns retained): code 'm'

		if (m.start<=r.exons[0]->end && m.end>=r.exons[jmax]->start)
			return 'm';

		for (int j=0;j<=jmax;j++) {
			//check if it's ~contained by an exon
			int exovlen=mseg.overlapLen(r.exons[j]);
			if (exovlen>0) {
				ovlen+=exovlen;
				if (m.start>r.exons[j]->start-4 && m.end<r.exons[j]->end+4) {
					return 'c'; //close enough to be considered contained in this exon
				}
			}
			if (j==jmax) break; //last exon here, no intron to check
			//check if it fully covers an intron (retained intron)
			if (m.start<r.exons[j]->end && m.end>r.exons[j+1]->start)
				return 'n';
			//check if it's fully contained by an intron
			if (m.end<r.exons[j+1]->start && m.start>r.exons[j]->end)
				return 'i';
			// check if it's a potential pre-mRNA transcript
			// (if overlaps this intron at least 10 bases)
			uint introvl=mseg.overlapLen(r.exons[j]->end+1, r.exons[j+1]->start-1);
			//iovlen+=introvl;
			if (introvl>=10 && mseg.len()>introvl+10) { rcode='e'; }
		} //for each ref exon
		if (rcode>0) return rcode;
		return 'o'; //plain overlap, uncategorized
	} //single-exon transfrag
	//-- multi-exon transfrag --
	int imax=m.exons.Count()-1;// imax>0 here
	if (jmax==0) { //single-exon reference overlap
		//any exon overlap?
		GSeg rseg(r.start, r.end);
		for (int i=0;i<=imax;i++) {
			//check if it's ~contained by an exon
			int exovlen=rseg.overlapLen(m.exons[i]);
			if (exovlen>0) {
				ovlen+=exovlen;
				if (r.start>m.exons[i]->start-4 && r.end<m.exons[i]->end+4) {
					return 'k'; //reference contained in this assembled exon
				}
			}
			if (i==imax) break;
			if (r.end<m.exons[i+1]->start && r.start>m.exons[i]->end)
				return 'y'; //ref contained in this transfrag intron
		}
		return 'o';
	}
	// * check if transfrag contained by a ref intron
	for (int j=0;j<jmax;j++) {
		if (m.end<r.exons[j+1]->start && m.start>r.exons[j]->end)
			return 'i';
	}
	if (m.exons[imax]->start<r.exons[0]->end) {
		//qry intron chain ends before ref intron chain starts
		//check if last qry exon plugs the 1st ref intron
		if (m.exons[imax]->start<=r.exons[0]->end &&
			m.exons[imax]->end>=r.exons[1]->start) return 'n';
		return 'o'; //only terminal exons overlap
	}
	else if (r.exons[jmax]->start<m.exons[0]->end) {
		//qry intron chain starts after ref intron chain ends
		//check if first qry exon plugs the last ref intron
		if (m.exons[0]->start<=r.exons[jmax-1]->end &&
			m.exons[0]->end>=r.exons[jmax]->start) return 'n';
		return 'o'; //only terminal exons overlap
	}
	//check intron chain overlap (match, containment, intron retention etc.)
	int i=1; //index of exon to the right of current qry intron
	int j=1; //index of exon to the right of current ref intron
	bool intron_conflict=false; //used for checking for retained introns
	//from here on we check all qry introns against ref introns
	bool junct_match=false; //true if at least a junction match is found
	bool ichain_match=true; //if there is intron (sub-)chain match, to be updated by any mismatch
	bool intron_ovl=false; //if any intron overlap is found
	bool intron_retention=false; //if any ref intron is covered by a qry exon
	int imfirst=0; //index of first intron match in query (valid>0)
	int jmfirst=0; //index of first intron match in reference (valid>0)
	int imlast=0;  //index of first intron match in query
	int jmlast=0;  //index of first intron match in reference
	//check for intron matches
	while (i<=imax && j<=jmax) {
		uint mstart=m.exons[i-1]->end;
		uint mend=m.exons[i]->start;
		uint rstart=r.exons[j-1]->end;
		uint rend=r.exons[j]->start;
		if (rend<mstart) { //qry intron starts after ref intron ends
			if (!intron_conflict && r.exons[j]->overlap(mstart+1, mend-1))
				intron_conflict=true;
			if (!intron_retention && rstart>=m.exons[i-1]->start)
				intron_retention=true;
			if (intron_ovl) ichain_match=false;
			j++;
			continue;
		} //no intron overlap, skipping ref intron
		if (rstart>mend) { //qry intron ends before ref intron starts
			//if qry intron overlaps the exon on the left, we have an intron conflict
			if (!intron_conflict && r.exons[j-1]->overlap(mstart+1, mend-1))
				intron_conflict=true;
			if (!intron_retention && rend<=m.exons[i]->end)
				intron_retention=true;
			if (intron_ovl) ichain_match=false;
			i++;
			continue;
		} //no intron overlap, skipping qry intron
		intron_ovl=true;
		//overlapping introns, test junction matching
		bool smatch=(mstart==rstart); //TODO: what if the introns differ just by 2 bases at one end?
		bool ematch=(mend==rend);
		if (smatch || ematch) junct_match=true;
		if (smatch && ematch) {
			//perfect match for this intron
			if (ichain_match) { //chain matching still possible
			  if (jmfirst==0) jmfirst=j;
			  if (imfirst==0) imfirst=i;
			  imlast=i;
			  jmlast=j;
			}
			i++; j++;
			continue;
		}
		//intron overlapping but with at least a junction mismatch
		intron_conflict=true;
		ichain_match=false;
		if (mend>rend) j++; else i++;
	} //while checking intron overlaps
	if (ichain_match) { //intron sub-chain match
		if (imfirst==1 && imlast==imax) { // qry full intron chain match
			if (jmfirst==1 && jmlast==jmax) return '='; //identical intron chains
			// -- qry intron chain is shorter than ref intron chain --
			int l_iovh=0;   // overhang of leftmost q exon left boundary beyond the end of ref intron to the left
			int r_iovh=0;   // same type of overhang through the ref intron on the right
			if (jmfirst>1 && r.exons[jmfirst-1]->start>m.start)
				l_iovh = r.exons[jmfirst-1]->start - m.start;
			if (jmlast<jmax && m.end > r.exons[jmlast]->end)
				r_iovh = m.end - r.exons[jmlast]->end;
			if (l_iovh<4 && r_iovh<4) return 'c';
		} else if ((jmfirst==1 && jmlast==jmax)) {//ref full intron chain match
			//check if the reference i-chain is contained in qry i-chain
			int l_jovh=0;   // overhang of leftmost q exon left boundary beyond the end of ref intron to the left
			int r_jovh=0;   // same type of overhang through the ref intron on the right
			if (imfirst>1 && m.exons[imfirst-1]->start>r.start)
				l_jovh = m.exons[imfirst-1]->start - r.start;
			if (imlast<imax && r.end > m.exons[imlast]->end)
				r_jovh = r.end - m.exons[imlast]->end;
			if (l_jovh<4 && r_jovh<4) return 'k'; //reverse containment
		}
	}
	//'=', 'c' and 'k' where checked and assigned, check for 'm' and 'n' before falling back to 'j'
	if (!intron_conflict && (m.start<=r.exons[0]->end && m.end>=r.exons[jmax]->start)) {
			return 'm';
	}
	if (intron_retention) return 'n';
	if (junct_match) return 'j';
	//we could have 'o' or 'y' here
	//any real exon overlaps?
	ovlen=m.exonOverlapLen(r);
	if (ovlen>4) return 'o';
	return 'y'; //all reference exons are within transfrag introns!
}
