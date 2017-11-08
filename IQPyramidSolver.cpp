#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <chrono>

#if defined(_WIN32) || defined(_WIN64)	// 在windows下所需的头文件
#include <numeric>
#include <Windows.h>
#endif

#include <boost/program_options.hpp>

//#undef USING_TBB	// 不使用TBB库
#define USING_TBB	// 使用TBB库

#ifdef USING_TBB
#include <tbb/tbb.h>
#include <tbb/concurrent_vector.h>
#include <tbb/atomic.h>
#endif

using namespace std;
namespace bpo = boost::program_options;

// 共有12片积木
static const int PIECES = 12;
// 每块积木的形状
// 用一个4*4的矩阵表示
// |A | B | C | D | E | F | G | H | I | J | K | L |
// ------------------------------------------------
// O   O   O   O   O   O   O   O   OO  O   OO   O
// O   OO  O   O   O   OO  O   OO  O   O   OO  OOO
// OO  OO  O   OO  OO      OOO  OO OO  O        O
//         OO  O    O                  O
// ------------------------------------------------
static const unsigned int PieceData[PIECES][4] = {
	{ 0b1000, 0b1000, 0b1100, 0b0000 },
	{ 0b1000, 0b1100, 0b1100, 0b0000 },
	{ 0b1000, 0b1000, 0b1000, 0b1100 },
	{ 0b1000, 0b1000, 0b1100, 0b1000 },
	{ 0b1000, 0b1000, 0b1100, 0b0100 },
	{ 0b1000, 0b1100, 0b0000, 0b0000 },
	{ 0b1000, 0b1000, 0b1110, 0b0000 },
	{ 0b1000, 0b1100, 0b0110, 0b0000 },
	{ 0b1100, 0b1000, 0b1100, 0b0000 },
	{ 0b1000, 0b1000, 0b1000, 0b1000 },
	{ 0b1100, 0b1100, 0b0000, 0b0000 },
	{ 0b0100, 0b1110, 0b0100, 0b0000 }
};
// 每块积木的不同形状的数目
static const int rotates[] = { 8, 8, 8, 8, 8, 4, 4, 4, 4, 2, 1, 1 };
// 并行化舞蹈链求解时广度优先分解的层数
static const int FACTOR = 3;

// 每块积木的代号,答案显示时用
static const string piece_map[PIECES] = { "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L" };
#if defined(_WIN32) || defined(_WIN64)
// windows下每块积木在控制台中显示字符的颜色控制
static const WORD console_color[PIECES] = {
	FOREGROUND_BLUE,
	FOREGROUND_GREEN,
	FOREGROUND_RED,
	FOREGROUND_BLUE | FOREGROUND_GREEN,
	FOREGROUND_BLUE | FOREGROUND_RED,
	FOREGROUND_GREEN | FOREGROUND_RED,
	FOREGROUND_BLUE | FOREGROUND_INTENSITY,
	FOREGROUND_GREEN | FOREGROUND_INTENSITY,
	FOREGROUND_RED | FOREGROUND_INTENSITY,
	FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
	FOREGROUND_BLUE | FOREGROUND_RED | FOREGROUND_INTENSITY,
	FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY,
};
#else
// 类unix系统中shell内每块积木在控制台中显示字符的颜色控制
static const string ansi_color[PIECES + 1] = {
	"\033[1;37m",
	"\033[1;31m",
	"\033[1;32m",
	"\033[0;32m",
	"\033[1;33m",
	"\033[0;33m",
	"\033[1;34m",
	"\033[0;34m",
	"\033[1;35m",
	"\033[0;35m",
	"\033[1;36m",
	"\033[0;36m",
	"\033[0m"
};
#endif

// 表示坐标点的数据结构
struct Point
{
	int x, y;
	Point(int x, int y) : x(x), y(y) {}
};

// 表示一个解中的一步
// 由于有12块积木,一个完整的解包含这12块积木的形状,位置
// 一个Step包含积木序号,形状序号,x和y坐标
// 因此一个完整的解包含12个Step
struct Step
{
	int block_index, shape_index, x, y;
	vector<int> indecies;
	Step(int block_index, int shape_index, int x, int y) : block_index(block_index), shape_index(shape_index), x(x), y(y) { indecies.clear(); }
};

// 每块积木的每个形状的数据
// 主要保存了形状在4*4矩阵中占据的点
class Piece
{
private:
	static const int WIDTH = 4;
	static const int HEIGHT = 4;
	vector<Point> points;

	// 使形状紧贴矩阵的左上角
	void Normalize()
	{
		int min_x = WIDTH, min_y = HEIGHT;
		for (Point p : points)
		{
			min_x = (std::min)(min_x, p.x);
			min_y = (std::min)(min_y, p.y);
		}
		for (Point &p : points)
			p = Point(p.x - min_x, p.y - min_y);
	}

public:
	const int block_index;
	int shape_index;

	// 用点阵数据初始化
	Piece(const unsigned int(&data)[HEIGHT], int block_index) : block_index(block_index)
	{
		points.clear();
		shape_index = 0;
		for (int i = 0; i < HEIGHT; i++)
			for (int j = 0; j < WIDTH; j++)
				if ((data[i] & (1u << (WIDTH - j - 1))) != 0)
					points.push_back(Point(j, i));
		Normalize();
	}

	// 水平翻转
	void Flip()
	{
		for (Point &p : points)
			p = Point(WIDTH - p.x - 1, p.y);
		Normalize();
		shape_index++;
	}

	// 顺时针旋转90度
	void Rotate()
	{
		for (Point &p : points)
			p = Point(HEIGHT - p.y - 1, p.x);
		Normalize();
		shape_index++;
	}

	// 形状占据矩阵点的数量
	int size() const { return (int)(points.size()); }

	// 形状占据矩阵点的集合
	vector<Point>& getPoints() { return points; }
};

// 棋盘图案的统一接口
class IPattern
{
public:
	// 图案中所有需要填充的位置数量
	virtual int size() const = 0;
	// 获取形状piece在图案中所有可能的位置
#ifdef USING_TBB
	virtual int GetValidSteps(Piece &piece, tbb::concurrent_vector<Step>& steps) const = 0;
#else
	virtual int GetValidSteps(Piece& piece, vector<Step>& steps) const = 0;
#endif
	// 依据图案的形状输出一个解
	virtual vector<vector<int> > FormatMatrix(const vector<Step>& solution) const = 0;
};

// 高=10,底边=10的三角形图案
class TrianglePattern : public IPattern
{
private:
	static const int ORDER = 10;
	vector<int> matrix;

public:
	TrianglePattern()
	{
		matrix.clear();
		int index = 0;
		for (int y = 0; y < ORDER; y++)
			for (int x = 0; x < ORDER; x++)
				if (x <= y)
					matrix.push_back(++index);
				else
					matrix.push_back(0);
	}

	int size() const { return ORDER * (ORDER + 1) / 2; }

#ifdef USING_TBB
	int GetValidSteps(Piece &piece, tbb::concurrent_vector<Step> &steps) const
#else
	int GetValidSteps(Piece& piece, vector<Step>& steps) const
#endif
	{
		int count = 0;
		for (int y = 0; y < ORDER; y++)
			for (int x = 0; x < ORDER; x++)
			{
				bool valid = true;
				for (Point p : piece.getPoints())
				{
					if (p.x + x < 0 || p.x + x >= ORDER) { valid = false; break; }
					if (p.y + y < 0 || p.y + y >= ORDER) { valid = false; break; }
					if (matrix[(p.y + y) * ORDER + p.x + x] == 0) { valid = false; break; }
				}
				if (valid)
				{
					Step step(piece.block_index, piece.shape_index, x, y);
					for (Point p : piece.getPoints())
						step.indecies.push_back(matrix[(p.y + y) * ORDER + p.x + x]);
					steps.push_back(step);
					count++;
				}
			}
		return count;
	}

	vector<vector<int> > FormatMatrix(const vector<Step>& solution) const
	{
		vector<int> piece_matrix(size() + 1, 0);
		for (Step step : solution)
			for (int index : step.indecies)
				piece_matrix[index] = step.block_index;

		vector<vector<int> > result;
		int index = 0;
		for (int y = 0; y < ORDER; y++)
		{
			result.push_back(vector<int>());
			for (int x = 0; x < ORDER; x++)
				if (x <= y)
					result[y].push_back(piece_matrix[++index]);
		}
		return result;
	}
};

// 宽=11,高=5的矩形图案
class RectanglePattern : public IPattern
{
private:
	static const int WIDTH = 11;
	static const int HEIGHT = 5;
	vector<int> matrix;

public:
	RectanglePattern()
	{
		matrix.clear();
		int index = 0;
		for (int y = 0; y < HEIGHT; y++)
			for (int x = 0; x < WIDTH; x++)
				matrix.push_back(++index);
	}

	int size() const { return WIDTH * HEIGHT; }

#ifdef USING_TBB
	int GetValidSteps(Piece &piece, tbb::concurrent_vector<Step> &steps) const
#else
	int GetValidSteps(Piece& piece, vector<Step>& steps) const
#endif
	{
		int count = 0;
		for (int y = 0; y < HEIGHT; y++)
			for (int x = 0; x < WIDTH; x++)
			{
				bool valid = true;
				for (Point p : piece.getPoints())
				{
					if (p.x + x < 0 || p.x + x >= WIDTH) { valid = false; break; }
					if (p.y + y < 0 || p.y + y >= HEIGHT) { valid = false; break; }
					if (matrix[(p.y + y) * WIDTH + p.x + x] == 0) { valid = false; break; }
				}
				if (valid)
				{
					Step step(piece.block_index, piece.shape_index, x, y);
					for (Point p : piece.getPoints())
						step.indecies.push_back(matrix[(p.y + y) * WIDTH + p.x + x]);
					steps.push_back(step);
					count++;
				}
			}
		return count;
	}

	vector<vector<int> > FormatMatrix(const vector<Step>& solution) const
	{
		vector<int> piece_matrix(size() + 1, 0);
		for (Step step : solution)
			for (int index : step.indecies)
				piece_matrix[index] = step.block_index;

		vector<vector<int> > result;
		int index = 0;
		for (int y = 0; y < HEIGHT; y++)
		{
			result.push_back(vector<int>());
			for (int x = 0; x < WIDTH; x++)
				result[y].push_back(piece_matrix[++index]);
		}
		return result;
	}
};

// 金字塔形图案
class PyramidPattern : public IPattern
{
private:
	const int ORDER;
	vector<vector<int> > floors;			// 所有的水平面
	vector<vector<int> > diagonals_left;	// 所有的135度纵切面
	vector<vector<int> > diagonals_right;	// 所有的45度纵切面

public:
	PyramidPattern(int order) : ORDER(order)
	{
		int index = 0;
		floors.resize(ORDER);
		for (int floor = 0; floor < ORDER; floor++)
		{
			floors[floor].clear();
			for (int y = 0; y <= floor; y++)
				for (int x = 0; x <= floor; x++)
					floors[floor].push_back(++index);
		}

		diagonals_left.resize(2 * ORDER - 1);
		diagonals_right.resize(2 * ORDER - 1);
		for (int plane = 0; plane < 2 * ORDER - 1; plane++)
		{
			diagonals_left[plane].clear();
			diagonals_right[plane].clear();
			int size = ORDER - std::abs(ORDER - 1 - plane);
			for (int y = 0; y < size; y++)
				for (int x = 0; x < size; x++)
				{
					if (x <= y)
					{
						int floor = ORDER - 1 - (y - x);
						int offset = plane - ORDER + 1;
						if (offset < 0)
						{
							diagonals_left[plane].push_back(floors[floor][x * (floor + 1) + floor + offset - x]);
							diagonals_right[plane].push_back(floors[floor][(floor + offset - x) * (floor + 1) + floor - x]);
						}
						else
						{
							diagonals_left[plane].push_back(floors[floor][(x + offset) * (floor + 1) + floor - x]);
							diagonals_right[plane].push_back(floors[floor][(floor - x) * (floor + 1) + floor - offset - x]);
						}
					}
					else
					{
						diagonals_left[plane].push_back(0);
						diagonals_right[plane].push_back(0);
					}
				}
		}
	}

	int size() const { return ORDER * (ORDER + 1) * (ORDER * 2 + 1) / 6; }

#ifdef USING_TBB
	int GetValidSteps(Piece& piece, tbb::concurrent_vector<Step>& steps) const
#else
	int GetValidSteps(Piece& piece, vector<Step>& steps) const
#endif
	{
		int count = 0;
		for (int floor = 0; floor < ORDER; floor++)
		{
			for (int y = 0; y <= floor; y++)
				for (int x = 0; x <= floor; x++)
				{
					bool valid = true;
					for (Point p : piece.getPoints())
					{
						if (p.x + x < 0 || p.x + x > floor) { valid = false; break; }
						if (p.y + y < 0 || p.y + y > floor) { valid = false; break; }
						if (floors[floor][(p.y + y) * (floor + 1) + p.x + x] == 0) { valid = false; break; }
					}
					if (valid)
					{
						Step step(piece.block_index, (floor << 3) | piece.shape_index, x, y);
						for (Point p : piece.getPoints())
							step.indecies.push_back(floors[floor][(p.y + y) * (floor + 1) + p.x + x]);
						steps.push_back(step);
						count++;
					}
				}
		}

		for (int plane = 0; plane < 2 * ORDER - 1; plane++)
		{
			int size = ORDER - std::abs(ORDER - 1 - plane);
			for (int y = 0; y < size; y++)
				for (int x = 0; x < size; x++)
				{
					bool valid = true;
					for (Point p : piece.getPoints())
					{
						if (p.x + x < 0 || p.x + x >= size) { valid = false; break; }
						if (p.y + y < 0 || p.y + y >= size) { valid = false; break; }
						if (diagonals_left[plane][(p.y + y) * size + p.x + x] == 0) { valid = false; break; }
					}
					if (valid)
					{
						Step step(piece.block_index, (1 << 6) | (plane << 3) | piece.shape_index, x, y);
						for (Point p : piece.getPoints())
							step.indecies.push_back(diagonals_left[plane][(p.y + y) * size + p.x + x]);
						steps.push_back(step);
						count++;
					}
				}
		}

		for (int plane = 0; plane < 2 * ORDER - 1; plane++)
		{
			int size = ORDER - abs(ORDER - 1 - plane);
			for (int y = 0; y < size; y++)
				for (int x = 0; x < size; x++)
				{
					bool valid = true;
					for (Point p : piece.getPoints())
					{
						if (p.x + x < 0 || p.x + x >= size) { valid = false; break; }
						if (p.y + y < 0 || p.y + y >= size) { valid = false; break; }
						if (diagonals_right[plane][(p.y + y) * size + p.x + x] == 0) { valid = false; break; }
					}
					if (valid)
					{
						Step step(piece.block_index, (1 << 7) | (plane << 3) | piece.shape_index, x, y);
						for (Point p : piece.getPoints())
							step.indecies.push_back(diagonals_right[plane][(p.y + y) * size + p.x + x]);
						steps.push_back(step);
						count++;
					}
				}
		}
		return count;
	}

	vector<vector<int> > FormatMatrix(const vector<Step>& solution) const
	{
		vector<int> piece_matrix(size() + 1, 0);
		for (Step step : solution)
		{
			for (int index : step.indecies)
			{
				piece_matrix[index] = step.block_index;
			}
		}

		vector<vector<int> > result;
		for (int i = 0; i < ORDER; i++)
		{
			result.push_back(vector<int>());
			for (int j = 0; j < ORDER; j++)
			{
				for (int k = 0; k <= j; k++)
				{
					if (j >= i)
						result[i].push_back(piece_matrix[floors[j][i * (j + 1) + k]]);
					else
						result[i].push_back(-1);
				}
				result[i].push_back(-1);;
			}
		}
		return result;
	}
};

// 舞蹈链算法实现
class DancingLinkX
{
private:
	vector<int> Left;
	vector<int> Right;
	vector<int> Up;
	vector<int> Down;

	vector<int> Column;
	vector<int> Row;

	vector<int> Count;
	vector<int> Header;

	int counter;

	vector<int> Answer;
	vector<vector<int>> Answers;

	int max_column;

public:
	// 构造函数
	DancingLinkX(int node_count, int row_count, int column_count, bool isComplete)
	{
		Left.resize(node_count, 0);
		Right.resize(node_count, 0);
		Up.resize(node_count, 0);
		Down.resize(node_count, 0);

		Column.resize(node_count, 0);
		Row.resize(node_count, 0);

		Count.resize(column_count + 1, 0);
		Header.resize(row_count + 1, 0);

		max_column = isComplete ? column_count : column_count - PIECES;

		for (int i = 0; i <= column_count; i++)
		{
			Left[i] = i == 0 ? column_count : i - 1;
			Right[i] = i == column_count ? 0 : i + 1;
			Up[i] = i;
			Down[i] = i;

			Column[i] = i;
			Row[i] = 0;

			Count[i] = 0;
		}
		counter = column_count;
	}

	// 从已有的DancingLinkX数据结构复制出一个对象
	DancingLinkX(const DancingLinkX& dlx)
	{
		Left = vector<int>(dlx.Left);
		Right = vector<int>(dlx.Right);
		Up = vector<int>(dlx.Up);
		Down = vector<int>(dlx.Down);

		Column = vector<int>(dlx.Column);
		Row = vector<int>(dlx.Row);

		Count = vector<int>(dlx.Count);
		Header = vector<int>(dlx.Header);

		counter = dlx.counter;

		Answer = vector<int>(dlx.Answer);

		max_column = dlx.max_column;
	}

	void Link(int column, int row);

	void KnownStep(int index);

	void Delete(int column);

	void Recover(int column);

	// 广度优先遍历level_needed层,分解原始的舞蹈链数据结构为一系列较简单的舞蹈链数据结构
	void Spread(int level, int level_needed, vector<vector<int>>& steps_list);

	// 深度优先遍历,递归查找所有解
	void Dance();

	vector<vector<int>> getResult() const
	{
		return Answers;
	}
};

void DancingLinkX::Link(int row, int column)
{
	counter++;
	Column[counter] = column;
	Row[counter] = row;
	Count[column]++;

	Up[counter] = Up[column];
	Down[counter] = column;
	Down[Up[column]] = counter;
	Up[column] = counter;

	if (Header[row] == 0)
	{
		Header[row] = counter;
		Left[counter] = counter;
		Right[counter] = counter;
	}
	else
	{
		Left[counter] = Left[Header[row]];
		Right[counter] = Header[row];
		Right[Left[Header[row]]] = counter;
		Left[Header[row]] = counter;
	}
}

void DancingLinkX::KnownStep(int index)
{
	Delete(Column[Header[index]]);
	Answer.push_back(index);
	for (int i = Right[Header[index]]; i != Header[index]; i = Right[i])
		Delete(Column[i]);
	return;
}

void DancingLinkX::Delete(int column)
{
	Right[Left[column]] = Right[column];
	Left[Right[column]] = Left[column];
	for (int i = Down[column]; i != column; i = Down[i])
		for (int j = Right[i]; j != i; j = Right[j])
		{
			Up[Down[j]] = Up[j];
			Down[Up[j]] = Down[j];
			Count[Column[j]]--;
		}
}

void DancingLinkX::Recover(int column)
{
	for (int i = Up[column]; i != column; i = Up[i])
		for (int j = Left[i]; j != i; j = Left[j])
		{
			Up[Down[j]] = j;
			Down[Up[j]] = j;
			Count[Column[j]]++;
		}
	Right[Left[column]] = column;
	Left[Right[column]] = column;
}

void DancingLinkX::Spread(int level, int level_needed, vector<vector<int>>& steps_list)
{
	if (level >= level_needed)
	{
		vector<int> steps = vector<int>(Answer);
		steps_list.push_back(steps);
		return;
	}
	int now = Right[0];
	int least_count = INT_MAX;
	for (int i = Right[0]; i != 0 && i <= max_column; i = Right[i])
		if (Count[i] < least_count)
		{
			least_count = Count[i];
			now = i;
		}
	Delete(now);
	for (int i = Down[now]; i != now; i = Down[i])
	{
		Answer.push_back(Row[i]);
		for (int j = Right[i]; j != i; j = Right[j])
			Delete(Column[j]);

		Spread(level + 1, level_needed, steps_list);

		for (int j = Left[i]; j != i; j = Left[j])
			Recover(Column[j]);
		Answer.pop_back();
	}
	Recover(now);
	return;
}

void DancingLinkX::Dance()
{
	int now = Right[0];
	if (now == 0 || now > max_column)
	{
		vector<int> result = vector<int>(Answer);
		Answers.push_back(result);
		return;
	}
	int least_count = INT_MAX;
	for (int i = Right[0]; i != 0 && i <= max_column; i = Right[i])
		if (Count[i] < least_count)
		{
			least_count = Count[i];
			now = i;
		}
	Delete(now);
	for (int i = Down[now]; i != now; i = Down[i])
	{
		Answer.push_back(Row[i]);
		for (int j = Right[i]; j != i; j = Right[j])
			Delete(Column[j]);

		Dance();

		for (int j = Left[i]; j != i; j = Left[j])
			Recover(Column[j]);
		Answer.pop_back();
	}
	Recover(now);
	return;
}

// 输出结果到控制台,不同积木用不同颜色表示
void OutputToConsole(const vector<vector<int> >& matrix)
{
	for (vector<int> line : matrix)
	{
		for (int block_index : line)
			if (block_index == -1)
				std::cout << " ";
			else
			{
#if defined(_WIN32) || defined(_WIN64)
				CONSOLE_SCREEN_BUFFER_INFO csbiInfo;
				HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
				GetConsoleScreenBufferInfo(handle, &csbiInfo);
				WORD wOldColorAttrs = csbiInfo.wAttributes;
				SetConsoleTextAttribute(handle, console_color[block_index]);
				std::cout << piece_map[block_index];
				SetConsoleTextAttribute(handle, wOldColorAttrs);
#else
				std::cout << ansi_color[block_index] << piece_map[block_index] << ansi_color[PIECES];
#endif
			}
		std::cout << endl;
	}
	std::cout << endl;
}

// 输出结果到文件,不需要彩色文本
void OutputToFile(const vector<vector<int> >& matrix, std::ofstream& fout)
{
	for (vector<int> line : matrix)
	{
		for (int block_index : line)
			if (block_index == -1)
				fout << " ";
			else
				fout << piece_map[block_index];
		fout << endl;
	}
	fout << endl;
}

int main(int argc, const char *argv[])
{
	// 提取和处理命令行参数
	string type, filename;
	int level;
	bpo::options_description desc("Allowed options");
	desc.add_options()("help,h", "display help message")
		("type,t", bpo::value<string>(&type), "the puzzle pattern type : [t|r|p4|p5]\nt: Triangle Pattern\nr: Rectangle Pattern\np4: 4 Level Pyramid Pattern\np5: 5 Level Pyramid Pattern")
		("output,o", bpo::value<string>(&filename), "output filename\nif not set, output to console")
		("level,l", bpo::value<int>(&level)->default_value(FACTOR), "spread level for parallelize: [1--12]")
		;

	bpo::variables_map vm;
	try
	{
		bpo::store(bpo::parse_command_line(argc, argv, desc), vm);
		bpo::notify(vm);
	}
	catch (const bpo::error &e)
	{
		std::cerr << endl << e.what() << endl << endl;
		std::cerr << desc << endl;
		return 1;
	}

	if (vm.count("help"))
	{
		std::cout << desc << endl;
		return 0;
	}

	IPattern * pattern = NULL;
	if (vm.count("type"))
	{
		if (type == "t")
		{
			cout << "Solving Triangle Pattern Puzzle." << endl;
			pattern = new TrianglePattern();
		}
		else if (type == "r")
		{
			cout << "Solving Rectangle Pattern Puzzle." << endl;
			pattern = new RectanglePattern();
		}
		else if (type == "p4")
		{
			cout << "Solving 4 Level Pyramid Pattern Puzzle." << endl;
			pattern = new PyramidPattern(4);
		}
		else if (type == "p5")
		{
			cout << "Solving 5 Level Pyramid Pattern Puzzle." << endl;
			pattern = new PyramidPattern(5);
		}
		else
		{
			std::cerr << "Not a known type." << endl;
			std::cerr << endl << desc << endl << endl;
			return 1;
		}
	}
	else
	{
		std::cerr << endl << desc << endl << endl;
		return 1;
	}

	if (vm.count("level"))
	{
		if (level < 1 || level > 12)
		{
			std::cout << "level should between 1 and 12." << endl;
			return 0;
		}
	}
	std::cout << "Spread Level: " << level << endl;

	// 开始计时
	auto start = chrono::system_clock::now();

	// 初始化所有的积木数据
	vector<Piece> pieces;
	int piece_node_count = 0;
	for (int block_index = 0; block_index < PIECES; block_index++)
	{
		Piece piece(PieceData[block_index], block_index);
		pieces.push_back(piece);
		piece_node_count += piece.size();
		switch (rotates[block_index])
		{
		case 8:
			for (int i = 0; i < 3; i++)
			{
				piece.Rotate();
				pieces.emplace_back(piece);
			}
			piece.Flip();
			pieces.emplace_back(piece);
			for (int i = 0; i < 3; i++)
			{
				piece.Rotate();
				pieces.emplace_back(piece);
			}
			break;
		case 4:
			for (int i = 0; i < 3; i++)
			{
				piece.Rotate();
				pieces.emplace_back(piece);
			}
			break;
		case 2:
			piece.Rotate();
			pieces.emplace_back(piece);
			break;
		case 1:
		default:
			break;
		}
	}

#ifdef USING_TBB
	// 使用TBB,并行执行

	// 获得每块积木的每个形状在图案中的每个可能的位置
	tbb::concurrent_vector<Step> steps;
	tbb::parallel_for_each(pieces.begin(), pieces.end(), [&](Piece& piece) { pattern->GetValidSteps(piece, steps); });

	// 计算舞蹈链数据结构初始化所需的节点数目
	int node_count = 0;
	typedef tbb::blocked_range<tbb::concurrent_vector<Step>::iterator> range_type;
	node_count = tbb::parallel_reduce(range_type(steps.begin(), steps.end()), 0, [&](range_type const&r, int init) {
		return std::accumulate(r.begin(), r.end(), init, [&](int value, Step& step) {
			return value + step.indecies.size() + 1;
		}); }, std::plus<int>());
	node_count += PIECES + pattern->size() + 1;

	// 初始化舞蹈链数据结构
	// 构造关系矩阵
	DancingLinkX dlx(node_count, (int)(steps.size()), (int)(pattern->size()) + PIECES, ((int)(pattern->size()) == piece_node_count));
	for (int i = 0; i < steps.size(); i++)
	{
		for (int index : steps[i].indecies)
			dlx.Link(i + 1, index);
		dlx.Link(i + 1, steps[i].block_index + pattern->size() + 1);
	}

	vector<vector<int> > steps_list;

	// 广度优先遍历,展开解空间树为一系列子树,供并行处理
	dlx.Spread(0, level, steps_list);

	tbb::concurrent_vector<vector<int> > results;
	tbb::atomic<int> count = 0;
	tbb::spin_mutex mtx;

	// 隐藏控制台光标,防止显示进度时光标闪烁
#if defined(_WIN32) || defined(_WIN64)
	HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
	CONSOLE_CURSOR_INFO cci;
	GetConsoleCursorInfo(handle, &cci);
	bool oldVisible = cci.bVisible;
	cci.bVisible = false;
	SetConsoleCursorInfo(handle, &cci);
#else
	cout << "\033[?25l" << flush;
#endif

	// 并行求解
	tbb::parallel_for_each(steps_list.begin(), steps_list.end(), [&](vector<int> steps) {
		DancingLinkX clone(dlx);
		for (int step : steps)
			clone.KnownStep(step);
		clone.Dance();
		for (vector<int> solution : clone.getResult())
			results.push_back(solution);

		// 显示进度
		int x = (++count) * 100 / (int)(steps_list.size());
		tbb::spin_mutex::scoped_lock lock(mtx);
		std::cout << "\r" << x << "% completed." << flush;
	});

	// 恢复控制台光标显示
#if defined(_WIN32) || defined(_WIN64)
	cci.bVisible = oldVisible;
	SetConsoleCursorInfo(handle, &cci);
	cout << "\r100% completed." << endl;
#else
	cout << "\033[?25h" << "\r100% completed." << endl;
#endif

	// 整理得到的所有解,排序
	tbb::concurrent_vector<vector<Step>> solutions;
	tbb::parallel_for_each(results.begin(), results.end(), [&](vector<int> result) {
		vector<Step> solution;
		for (int index : result)
			solution.push_back(steps[index - 1]);
		std::sort(solution.begin(), solution.end(), [&](const Step& step1, const Step& step2) {
			return step1.block_index < step2.block_index;
		});
		solutions.push_back(solution);
	});
	tbb::parallel_sort(solutions.begin(), solutions.end(), [&](const vector<Step>& solution1, const vector<Step>& solution2) {
		for (int i = 0; i < PIECES; i++)
		{
			if (solution1[i].shape_index != solution2[i].shape_index)
				return solution1[i].shape_index < solution2[i].shape_index;
			else if (solution1[i].x != solution2[i].x)
				return solution1[i].x < solution2[i].x;
			else if (solution1[i].y != solution2[i].y)
				return solution1[i].y < solution2[i].y;
		}
		return false;
	});

#else

	// 不使用TBB,单核心执行

	vector<Step> steps;
	for (Piece piece : pieces)
		pattern->GetValidSteps(piece, steps);

	int node_count = 0;
	node_count = std::accumulate(steps.begin(), steps.end(), 0, [&](int value, Step& step) {
		return value + step.indecies.size() + 1;
	});
	node_count += PIECES + pattern->size() + 1;

	DancingLinkX dlx(node_count, steps.size(), pattern->size() + PIECES, (pattern->size() == piece_node_count));
	for (int i = 0; i < steps.size(); i++)
	{
		for (int index : steps[i].indecies)
			dlx.Link(i + 1, index);
		dlx.Link(i + 1, steps[i].block_index + pattern->size() + 1);
	}

	vector<vector<int> > steps_list;

	dlx.Spread(0, level, steps_list);

	vector<vector<int> > results;
	int count = 0;
#if defined(_WIN32) || defined(_WIN64)
	HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
	CONSOLE_CURSOR_INFO cci;
	GetConsoleCursorInfo(handle, &cci);
	bool oldVisible = cci.bVisible;
	cci.bVisible = false;
	SetConsoleCursorInfo(handle, &cci);
#else
	cout << "\033[?25l" << flush;
#endif

	for (vector<int> steps : steps_list)
	{
		DancingLinkX clone(dlx);
		for (int step : steps)
			clone.KnownStep(step);
		clone.Dance();
		for (vector<int> solution : clone.getResult())
			results.push_back(solution);

		int x = (++count) * 100 / steps_list.size();
		std::cout << "\r" << x << "% completed." << flush;
	}
#if defined(_WIN32) || defined(_WIN64)
	cci.bVisible = oldVisible;
	SetConsoleCursorInfo(handle, &cci);
	cout << endl;
#else
	cout << "\033[?25h" << endl;
#endif

	vector<vector<Step> > solutions;
	for (vector<int> result : results)
	{
		vector<Step> solution;
		for (int index : result)
			solution.push_back(steps[index - 1]);
		std::sort(solution.begin(), solution.end(), [&](const Step& step1, const Step& step2) {
			return step1.block_index < step2.block_index;
		});
		solutions.push_back(solution);
	}
	std::sort(solutions.begin(), solutions.end(), [&](const vector<Step>& solution1, const vector<Step>& solution2) {
		for (int i = 0; i < PIECES; i++)
		{
			if (solution1[i].shape_index != solution2[i].shape_index)
				return solution1[i].shape_index < solution2[i].shape_index;
			else if (solution1[i].x != solution2[i].x)
				return solution1[i].x < solution2[i].x;
			else if (solution1[i].y != solution2[i].y)
				return solution1[i].y < solution2[i].y;
		}
		return false;
});
#endif

	// 停止计时
	auto end = chrono::system_clock::now();
	auto duration = chrono::duration_cast<chrono::microseconds>(end - start);
	// 显示消耗时间
	cout << "Time Spend: " << double(duration.count()) * chrono::microseconds::period::num / chrono::microseconds::period::den << " Seconds" << endl;
	if (solutions.size() == 0)
		std::cout << "No solution found." << endl;
	else
		std::cout << solutions.size() << " solution(s) found." << endl;

	if (vm.count("output"))
	{
		// 输出结果到文件
		std::ofstream  fout(filename, ios::out);
		cout << "Outputing solution(s) to " << filename << "..." << endl;
		if (solutions.size() == 0)
			fout << "No solution found." << endl;
		else
		{
			fout << solutions.size() << " solution(s) found." << endl << endl;
			for (vector<Step> solution : solutions)
				OutputToFile(pattern->FormatMatrix(solution), fout);
		}
		cout << "Output Complete." << endl;
	}
	else
	{
		// 输出结果到控制台
		cout << endl;
		for (vector<Step> solution : solutions)
			OutputToConsole(pattern->FormatMatrix(solution));
	}

	delete pattern;
	return 0;
}