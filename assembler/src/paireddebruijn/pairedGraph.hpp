#include "vector"
#include "sequence.hpp"
#include "common.hpp"
#include "graphVisualizer.hpp"
#include "logging.hpp"
//#include "hashTable.h"
using namespace std;

#ifndef PAIREDGRAPH_H_
#define PAIREDGRAPH_H_

namespace paired_assembler {

//typedef int Kmer;

//class Sequence {
//	char *_nucleotides;
//	short _length;
//public:
//	Sequence(char *nucleotides, short length) : _nucleotides(nucleotides), _length(length)
//	{}
//	char operator[](const int &index);
//};

class Vertex;

//struct Arc {
//	short _coverage;
//	Vertex* _head;
//};
class VertexPrototype {
public:
	ll upper;
	Sequence *lower;
	int VertexId;
	bool used;
	int coverage;
	VertexPrototype(Sequence *lower_, int id, int coverage_ = 1);
	VertexPrototype(ll upper_, Sequence *lower_, int id, int coverage_ = 1);
	~VertexPrototype() {
		delete lower;
	}
};

class EdgePrototype {
public:
	EdgePrototype(Sequence *lower_, int start_, int coverage_ = 1) {
		lower = lower_;
		VertexId = start_;
		used = false;
		coverage = coverage_;
	}
	Sequence *lower;
	int VertexId;
	bool used;
	int coverage;
};
/*
 * length- including one vertex.
 * upper and lower including both in and out vertex
 *
 *
 *
 */
class Edge {
	//	int _coverage;
public:
	Sequence *upper;
	Sequence *lower;
	int length;
	int FromVertex;
	int ToVertex;
	int EdgeId;
	int coverage;
	Edge(Edge &e) {
		length = e.length;
		FromVertex = e.FromVertex;
		ToVertex = e.ToVertex;
		EdgeId = e.EdgeId;
		coverage = e.coverage;
		upper = new Sequence(const_cast<Sequence&> (*e.upper));
		lower = new Sequence(const_cast<Sequence&> (*e.lower));
	}
	//	Vertex(int coverage, int length, Sequence *kmer, Sequence *pair, bool direction, int delta_d);
	void ExpandRight(Edge &newRight) {
		ToVertex = newRight.ToVertex;
		if (newRight.length > 0) {
			length = length + newRight.length;
			string toOut = newRight.upper->str();
			assert(k-1 < toOut.length());
			upper = new Sequence(
					upper->str() + newRight.upper->Subseq(k - 1).str());

			toOut = newRight.lower->str();
			assert(l-1 < toOut.length());
			lower = new Sequence(
					lower->str() + newRight.lower->Subseq(l - 1).str());

		}
	}
	void shortenEdge(int toCut, int direction) {
		if (toCut < length) {
			if (direction == OUT_EDGE) {
				upper = new Sequence(upper->Subseq(toCut, length));
				lower = new Sequence(lower->Subseq(toCut, length));
			} else {
				upper = new Sequence(upper->Subseq(0, upper->size() - toCut));
				lower = new Sequence(lower->Subseq(0, lower->size() - toCut));
			}
			length -= toCut;
		}
	}
	void ExpandLeft(Edge &newLeft) {
		FromVertex = newLeft.FromVertex;
		if (newLeft.length > 0) {
			length = length + newLeft.length;
			upper = new Sequence(
					newLeft.upper->Subseq(0, newLeft.length).str()
							+ upper->str());
			lower = new Sequence(
					newLeft.lower->Subseq(0, newLeft.length).str()
							+ lower->str());
		}
	}
	Edge(Sequence *up, Sequence *low, int from, int to, int len, int id,
			int cov = 1) {
		upper = up;
		lower = low;
		FromVertex = from;
		ToVertex = to;
		length = len;
		EdgeId = id;
		coverage = cov;
	}

	~Edge() {
		//		cerr << "destructing" << upper->str() << endl;
		if (upper != lower) {
			delete upper;
			delete lower;
		} else
			delete upper;
	}
};

inline int edgeRealId(int id, longEdgesMap &longEdges) {
	int res = id;
	while (longEdges[res]->EdgeId != res) {
		res = longEdges[res]->EdgeId;
	}
	return res;
}

template<typename tVertex>
class IVertexIterator {
	virtual bool hasNext() = 0;
	virtual tVertex next() = 0;
};

template<typename tVertex, typename tEdge>
class IPairedGraph {
public:
	/*Better way to do it is to implement begin() and end() functions which would return pointers to start and
	 * end of lists of neighbours but it is not possible because of the order of dimentions in edgeIds and
	 * because those are edge ids in array instead of pointers
	 */
	virtual int rightDegree(tVertex vertex) = 0;
	virtual int leftDegree(tVertex vertex) = 0;
	int degree(tVertex vertex, int direction) {
		if (direction == RIGHT)//RIGHT = 1
			return rightDegree(vertex);
		else if (direction == LEFT)//LEFT = -1
			return leftDegree(vertex);
	}

	virtual tEdge rightEdge(tVertex vertex, int number) = 0;
	virtual tEdge leftEdge(tVertex vertex, int number) = 0;
	tEdge neighbour(tVertex vertex, int number, int direction) {
		if (direction == RIGHT)//RIGHT = 1
			return getRightNeighbour(vertex);
		else if (direction == LEFT)//LEFT = -1
			return getRightNeighbour(vertex);
	}

	virtual IVertexIterator<tVertex> *vertexIterator() = 0;

	//In order to add edge to graph one should create this edge first!
	virtual tEdge addEdge(tEdge newEdge) = 0;
	virtual void removeEdge(tEdge edge) = 0;

	//create ne vertex, adds it to graph and return
	virtual tVertex addVertex(tVertex) = 0;
	//add adjecent edges should be removed as well
	virtual void removeVertex(tVertex vertex) = 0;
	virtual tEdge merge(tEdge edge1, tEdge edge2) = 0;
	virtual pair<tEdge, tEdge> splitEdge(tEdge edge, int position) = 0;

	virtual tVertex glueVertices(tVertex vertex1, tVertex vertex2) = 0;
	//glue edges, there start and end vertices
	virtual tEdge glueEdges(tEdge edge1, tEdge edge2) = 0;
	//seperate edges  adjecent to the vertex
	virtual void unGlueEdges(tVertex vertex) = 0;
	virtual void unGlueEdgesLeft(tVertex vertex) = 0;
	virtual void unGlueEdgesRight(tVertex vertex) = 0;
};

class PairedGraphData {
public:
	//0 - in-degrees
	//1 -out-degrees
	int degrees[MAX_VERT_NUMBER][2];//, outD[MAX_VERT_NUMBER][2];
	int edgeIds[MAX_VERT_NUMBER][MAX_DEGREE][2];
	//	void recreateVerticesInfo(int vertCount, longEdgesMap &longEdges);
	vector<VertexPrototype *> vertexList_;
	longEdgesMap longEdges;
	verticesMap verts;
	int VertexCount;
	int EdgeId;
	PairedGraphData() {
		VertexCount = 0;
		EdgeId = 0;
		cerr << "VAH Paired created" << endl;
	}
	//	void RebuildVertexMap(void);
};

class VertexIterator: public IVertexIterator<VertexPrototype *> {
	friend class PairedGraphData;
private:
	int currentVertex_;
	PairedGraphData *graph_;
public:
	VertexIterator(PairedGraphData *graph) {
		currentVertex_ = 0;
	}

	virtual bool hasNext() {
		while (currentVertex_ < graph_->VertexCount
				&& graph_->degrees[currentVertex_][0]
						+ graph_->degrees[currentVertex_][1] == 0) {
			currentVertex_++;
		}
		return currentVertex_ < graph_->VertexCount;
	}

	virtual VertexPrototype *next() {
		if(!hasNext())
			assert(false);
		VertexPrototype *result = graph_->vertexList_[currentVertex_];
		currentVertex_++;
		return result;
	}
};

class PairedGraph: public PairedGraphData, public IPairedGraph<VertexPrototype *, Edge *> {
private:
	/**Method takes direction as RIGHT or LEFT, which are +1 and -1 and returns corresponding index
	 * for arrays in PairedGraphData which is 0 for LEFT and 1 for RIGHT
	 * @param direction LEFT or RIGHT
	 */
	inline int directionToIndex(int direction);

	//TODO replace these methods with transferEdgeAdjesency!!
	/**
	 * Method deletes @edge from the list of edges adjecent to @vertex
	 * @param vertex Vertex to delete adjacent edge from
	 * @param edge Edge to delete
	 * @direction Direction from which edge should be deleted. @direction can be LEFT or RIGHT.
	 */
	void removeEdgeVertexAdjacency(VertexPrototype *vertex, Edge *edge, int direction);

	/**
	 * Method adds @edge to the list of edges adjecent to @vertex. This method works in O(number of neighbours)
	 * time.
	 * @param vertex Vertex to add new adjacent edge to
	 * @param edge Edge to add
	 * @direction Direction from which edge is added to vertex. @direction can be LEFT or RIGHT.
	 */
	void addEdgeVertexAdjacency(VertexPrototype *vertex, Edge *edge, int direction);
public:

	//TODO C-style outcoming and incoming edge iterators should be added!!!

	/**
	 *Method returns number of outgoing edges for @vertex
	 */
	virtual int rightDegree(VertexPrototype *vertex);

	/**
	 *Method returns number of incoming edges for @vertex
	 */
	virtual int leftDegree(VertexPrototype *vertex);

	/**
	 * Method returns outcoming edge for given vertex with a given number
	 */
	virtual Edge *rightEdge(VertexPrototype *vertex, int number);

	/**
	 * Method returns incoming edge for given vertex with a given number
	 */
	virtual Edge *leftEdge(VertexPrototype *vertex, int number);

	//TODO Replace with C-stype iterator
	//TODO Make it return iterator instead of iterator *.
	//This is very bad method!!!!
	virtual VertexIterator *vertexIterator();

	/**
	 * Method adds edge to graph and updates all data stored in graph correspondingly.
	 *@param newEdge edge with any id value.
	 *@return the same Edge. After the edge is added to graph it is assigned with new id value
	 */
	virtual Edge *addEdge(Edge *newEdge);

	/*
	 * Method removes edge from graph, deletes @edge object and all its contents including Sequences stored in it.
	 * @param edge edge to be deleted
	 */
	virtual void removeEdge(Edge *edge);

	/**
	 * Method adds vertex to graph and updates all data stored in graph correspondingly. @newVertex is supposed
	 * to have upper sequence defined otherwise graph data may become inconsistent.
	 *@param newVertex vertex with any id value.
	 *@return the same vertex. After the vertex is added to graph it is assigned with new id value
	 */
	virtual VertexPrototype *addVertex(VertexPrototype *newVertex);

	/*
	 * Method removes vertex from graph, deletes @vertex object and all its contents including Sequences stored
	 * in it.
	 * @param vertex vertex to be deleted
	 */
	virtual void removeVertex(VertexPrototype *vertex);

	/**
	 * Method merges two edges into new one and deletes old edges.
	 * @param edge1 left edge to be merged
	 * @param edge2 right edge to be merged
	 * @return merged edge
	 */
	virtual Edge *merge(Edge *edge1, Edge *edge2);

	/**
	 * Method splits edge in two at given position
	 * @param edge edge to be split
	 * @param position position to split edge. Can not be less or equal to 0 or larger or equal than length of
	 * @return Two edges created
	 */
	virtual pair<Edge *, Edge *> splitEdge(Edge *edge, int position);

	/**
	 * Method transfers all connections of @vertex2 to @vertex1 and removes @vertex2
	 * @param vertex1 vertex to be glued to
	 * @param vertex2 vertex to be removed
	 * @return vertex which is @vertex1 glued to @vertex2. In this implementation it is @vertex1
	 */
	virtual VertexPrototype *glueVertices(VertexPrototype *vertex1, VertexPrototype *vertex2);

	/**
	 * Method glues edges, there start and end points. Currently implemented as gluing of start and end vertices
	 * and then deletion of @edges
	 * @param edge1 first edge to be glued
	 * @param edge2 second edge to be glued
	 * @return resulting edge
	 */
	virtual Edge *glueEdges(Edge *edge1, Edge *edge2);

	/**
	 * Method checks if given vertex has incoming or outcoming edge equal to 1 and in this case removes this
	 * vertex from graph and creates new edges which are concatenations of incoming and outcoming edges of
	 * the vertex.
	 *@vertex vertex edges adjecent to which should be processed
	 */
	virtual void unGlueEdges(VertexPrototype *vertex);

	/**
	 * Method checks if given vertex has incoming degree equal to 1 and in this case removes this
	 * vertex from graph and creates new edges which are concatenations of the incoming edge with all
	 * outcoming edges of the vertex.
	 *@vertex vertex edges adjecent to which should be processed
	 */
	virtual void unGlueEdgesLeft(VertexPrototype *vertex);

	/**
	 * Method checks if given vertex has outcoming degree equal to 1 and in this case removes this
	 * vertex from graph and creates new edges which are concatenations of the all incoming edges with the
	 * outcoming edge of the vertex.
	 *@vertex vertex edges adjecent to which should be processed
	 */
	virtual void unGlueEdgesRight(VertexPrototype *vertex);

	void recreateVerticesInfo(int vertCount, longEdgesMap &longEdges);

	void RebuildVertexMap(void);
};

int storeVertex(gvis::GraphPrinter<int> &g, PairedGraph &graph, ll newKmer,
		Sequence* newSeq);
int storeVertex(PairedGraph &graph, ll newKmer, Sequence* newSeq);
int storeVertex(PairedGraph &graph, ll newKmer, Sequence* newSeq, int VertNum);
void resetVertexCount(PairedGraph &graph);

}
#endif /* PAIREDGRAPH_H_ */
